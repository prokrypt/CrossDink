#include "HalPowerManager.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <PowerCounters.h>
#include <PowerManager.h>
#include <WiFi.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <soc/soc_caps.h>

#include <cassert>
#include <cstring>

#include "HalGPIO.h"

HalPowerManager powerManager;  // Singleton instance

namespace {
// Up to 30 ms for the driver to report Wi-Fi off, polled in 5 ms steps instead of a fixed wait.
void waitWiFiModeNull() {
  for (int i = 0; i < 6 && WiFi.getMode() != WIFI_MODE_NULL; ++i) {
    delay(5);
  }
}

void disableWiFiBeforeDeepSleep() {
  const wifi_mode_t wifiMode = WiFi.getMode();
  if (wifiMode == WIFI_MODE_NULL) {
    return;
  }

  LOG_DBG("PWR", "Disabling WiFi before deep sleep (mode=%d)", static_cast<int>(wifiMode));
  if (wifiMode & WIFI_MODE_AP) {
    WiFi.softAPdisconnect(true);
  }
  if (wifiMode & WIFI_MODE_STA) {
    WiFi.disconnect(true);
  }
  waitWiFiModeNull();
  WiFi.mode(WIFI_OFF);
  waitWiFiModeNull();
}

// [SLP] t columns in sleep-path order; each step adds its time to one column.
constexpr const char* kSleepCols[] = {"request", "draw",    "writes", "shutdown", "tilt",  "refresh",
                                      "fade",    "busters", "dslp",   "wifi",     "rails", "release"};
constexpr unsigned kSleepColCount = sizeof(kSleepCols) / sizeof(kSleepCols[0]);
constexpr struct {
  const char* step;
  uint8_t col;
} kSleepStepCols[] = {
    {"kosync push", 0},      {"activity exit", 0},
    {"sleep draw", 1},       {"sleep writes", 2},
    {"storage shutdown", 3}, {"tilt sleep", 4},
    {"sleep refresh", 5},    {"nvs, frontlight", 6},
    {"ghost busters", 7},    {"display sleep", 8},
    {"wifi off", 9},         {"serial end", 9},
    {"rails off", 10},       {"power button release", 11},
};

// RTC noinit (survives deep sleep and restarts, random after power loss): the
// magic gates it, and the boot clears it, so a sleep entry that restarted
// instead never prints.
struct SleepSteps {
  static constexpr uint32_t MAGIC = 0x534C5431;  // "SLT1"
  uint32_t magic;
  uint32_t startMs;
  uint32_t stepMs;
  uint32_t totalMs;  // 0 until "deep sleep start"
  int8_t col;        // column of the step in progress, -1 for none
  uint32_t ms[kSleepColCount];
};
RTC_NOINIT_ATTR SleepSteps rtcSleepSteps;
}  // namespace

void HalPowerManager::begin() {
  if (BoardConfig::ACTIVE.batteryAdc >= 0) {
    pinMode(BoardConfig::ACTIVE.batteryAdc, INPUT);
  }
  normalFreq = getCpuFrequencyMhz();
  modeMutex = xSemaphoreCreateMutex();
  assert(modeMutex != nullptr);

#if CONFIG_PM_ENABLE
  if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "crossdink-active", &cpuFreqLock) != ESP_OK) {
    LOG_ERR("PWR", "Failed to create CPU frequency lock; device will run at the DFS floor");
    cpuFreqLock = nullptr;
  } else {
    // Matches the initial isLowPower == false: the device boots active.
    esp_pm_lock_acquire(cpuFreqLock);
    cpuFreqLockHeld = true;
    PowerCounters::maxClock(true);
  }
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "epd-refresh", &displayPmLock) != ESP_OK) {
    LOG_ERR("PWR", "Failed to create display no-light-sleep lock; refresh may light-sleep");
    displayPmLock = nullptr;
  }
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usb-drive", &usbDrivePmLock) != ESP_OK) {
    LOG_ERR("PWR", "Failed to create USB Drive no-light-sleep lock; USB Drive may drop off the host");
    usbDrivePmLock = nullptr;
  }
  esp_pm_config_t pmConfig = {};
  pmConfig.max_freq_mhz = normalFreq;
  pmConfig.min_freq_mhz = DFS_MIN_FREQ;
  // Tickless idle light-sleeps whenever every task is blocked and no PM lock is
  // held. HalDisplay's busy-wait hooks (beginDisplayBusyWait/endDisplayBusyWait)
  // hold displayPmLock during EPD refresh so it stays out of that window, except
  // where refreshLightSleep lets a BUSY-pin wake end the sleep.
  pmConfig.light_sleep_enable = true;
  const esp_err_t pmErr = esp_pm_configure(&pmConfig);
  if (pmErr != ESP_OK) {
    LOG_ERR("PWR", "esp_pm_configure failed (%d); auto light sleep disabled", static_cast<int>(pmErr));
  } else {
    LOG_INF("PWR", "Auto light sleep enabled (%d-%d MHz)", pmConfig.min_freq_mhz, pmConfig.max_freq_mhz);
  }
#endif
}

#if CONFIG_PM_ENABLE
// Caller holds modeMutex.
void HalPowerManager::syncCpuFreqLock() {
  if (cpuFreqLock == nullptr) return;
  // Background work keeps full speed through panel BUSY waits too: it runs on
  // its own while another task waits.
  const bool wanted = !isLowPower && (!displayBusyWaitActive || backgroundWorkCount > 0);
  if (wanted == cpuFreqLockHeld) return;
  if (wanted) {
    esp_pm_lock_acquire(cpuFreqLock);
  } else {
    esp_pm_lock_release(cpuFreqLock);
  }
  cpuFreqLockHeld = wanted;
  PowerCounters::maxClock(wanted);
}
#endif

void HalPowerManager::beginDisplayBusyWait() {
#if CONFIG_PM_ENABLE
  displayPmLockHeld = displayPmLock != nullptr && !refreshLightSleep;
  if (displayPmLockHeld) esp_pm_lock_acquire(displayPmLock);
  // The panel runs its waveform on its own; the CPU only waits for BUSY, so
  // let DFS drop it to the floor for the duration. displayPmLock still keeps
  // it out of light sleep.
  if (modeMutex != nullptr) xSemaphoreTake(modeMutex, portMAX_DELAY);
  displayBusyWaitActive = true;
  syncCpuFreqLock();
  if (modeMutex != nullptr) xSemaphoreGive(modeMutex);
#endif
}

void HalPowerManager::endDisplayBusyWait() {
#if CONFIG_PM_ENABLE
  if (modeMutex != nullptr) xSemaphoreTake(modeMutex, portMAX_DELAY);
  displayBusyWaitActive = false;
  syncCpuFreqLock();
  if (modeMutex != nullptr) xSemaphoreGive(modeMutex);
  if (displayPmLockHeld) esp_pm_lock_release(displayPmLock);
  displayPmLockHeld = false;
#endif
}

void HalPowerManager::beginDisplayRefreshHold() {
#if CONFIG_PM_ENABLE
  if (displayPmLock != nullptr) esp_pm_lock_acquire(displayPmLock);
#endif
}

void HalPowerManager::endDisplayRefreshHold() {
#if CONFIG_PM_ENABLE
  if (displayPmLock != nullptr) esp_pm_lock_release(displayPmLock);
#endif
}

void HalPowerManager::beginBackgroundWork() {
  if (modeMutex == nullptr) return;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  ++backgroundWorkCount;
#if CONFIG_PM_ENABLE
  syncCpuFreqLock();
#endif
  xSemaphoreGive(modeMutex);
  setPowerSaving(false);
}

void HalPowerManager::endBackgroundWork() {
  if (modeMutex == nullptr) return;
  xSemaphoreTake(modeMutex, portMAX_DELAY);
  if (backgroundWorkCount > 0) --backgroundWorkCount;
#if CONFIG_PM_ENABLE
  syncCpuFreqLock();
#endif
  xSemaphoreGive(modeMutex);
  // The main loop re-enters power saving on its next idle tick.
}

void HalPowerManager::setUsbDriveActive(bool active) {
#if CONFIG_PM_ENABLE
  if (usbDrivePmLock == nullptr || usbDrivePmLockHeld == active) return;
  if (active) {
    esp_pm_lock_acquire(usbDrivePmLock);
  } else {
    esp_pm_lock_release(usbDrivePmLock);
  }
  usbDrivePmLockHeld = active;
#else
  (void)active;
#endif
}

void HalPowerManager::setPowerSaving(bool enabled) {
  if (normalFreq <= 0) {
    return;  // invalid state
  }

  if (modeMutex != nullptr) {
    xSemaphoreTake(modeMutex, portMAX_DELAY);
  }

#if CONFIG_PM_ENABLE
  const bool radioMayIdle = radioIdleSleepAllowed;
#else
  // Without PM, power saving means LOW_POWER_FREQ, too slow to keep Wi-Fi up.
  const bool radioMayIdle = false;
#endif
  auto wifiMode = WiFi.getMode();
  if (wifiMode != WIFI_MODE_NULL && !radioMayIdle) {
    // Wifi is active, force disabling power saving
    enabled = false;
  }
  if (backgroundWorkCount > 0) enabled = false;

  const bool locked = lockCount > 0;

  if (!locked && enabled && !isLowPower) {
#if CONFIG_PM_ENABLE
    // DFS owns the clock here: dropping the lock is what lets the CPU fall to
    // DFS_MIN_FREQ and lets the idle task light-sleep between loop ticks.
    isLowPower = true;
    syncCpuFreqLock();
#else
    if (!setCpuFrequencyMhz(LOW_POWER_FREQ)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", LOW_POWER_FREQ);
      if (modeMutex != nullptr) {
        xSemaphoreGive(modeMutex);
      }
      return;
    }
#endif
    isLowPower = true;

  } else if ((!enabled || locked) && isLowPower) {
#if CONFIG_PM_ENABLE
    isLowPower = false;
    syncCpuFreqLock();
#else
    if (!setCpuFrequencyMhz(normalFreq)) {
      LOG_DBG("PWR", "Failed to set CPU frequency = %d MHz", normalFreq);
      if (modeMutex != nullptr) {
        xSemaphoreGive(modeMutex);
      }
      return;
    }
#endif
    isLowPower = false;
  }

  if (modeMutex != nullptr) {
    xSemaphoreGive(modeMutex);
  }

  // Otherwise, no change needed
}

void HalPowerManager::setSleepStep(const char* step) {
  sleepStep = step;
  SleepSteps& s = rtcSleepSteps;
  const uint32_t now = millis();
  if (s.magic != SleepSteps::MAGIC) {
    s = {};
    s.magic = SleepSteps::MAGIC;
    s.startMs = now;
    s.col = -1;
  }
  if (s.col >= 0 && static_cast<unsigned>(s.col) < kSleepColCount) s.ms[s.col] += now - s.stepMs;
  s.stepMs = now;
  s.col = -1;
  for (const auto& m : kSleepStepCols) {
    if (strcmp(m.step, step) == 0) {
      s.col = static_cast<int8_t>(m.col);
      break;
    }
  }
  if (strcmp(step, "deep sleep start") == 0) s.totalMs = (now - s.startMs) | 1;
}

void HalPowerManager::logLastSleepSteps() {
  SleepSteps& s = rtcSleepSteps;
  if (s.magic == SleepSteps::MAGIC && s.totalMs != 0) {
    char cols[192];
    size_t pos = 0;
    cols[0] = '\0';
    for (unsigned i = 0; i < kSleepColCount; i++) {
      const int n =
          snprintf(cols + pos, sizeof(cols) - pos, "%s=%lu ", kSleepCols[i], static_cast<unsigned long>(s.ms[i]));
      if (n < 0 || static_cast<size_t>(n) >= sizeof(cols) - pos) break;
      pos += static_cast<size_t>(n);
    }
    LOG_INF("SLP", "t %stotal=%lu", cols, static_cast<unsigned long>(s.totalMs));
  }
  s.magic = 0;
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio, bool fullWake) const {
  setSleepStep("wifi off");
  disableWiFiBeforeDeepSleep();

#ifdef ENABLE_SERIAL_LOG
  // Tear down HWCDC so the host sees a clean disconnect and the peripheral
  // doesn't hold power domains that interfere with USB-powered GPIO wake.
  // logSerial is the raw HWCDC reference; Serial is the MySerialImpl proxy
  // (which doesn't expose end()).
  setSleepStep("serial end");
  logSerial.end();
#endif

#if !SOC_PM_SUPPORT_EXT1_WAKEUP
  // Release every configured battery latch. BoardConfig owns the pin mapping;
  // the collision guard prevents a stale/mismatched profile from driving a
  // display or SD bus pin low and holding it through sleep.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || BoardConfig::latchConflictsWithBus(pin)) continue;
    const auto latch = static_cast<gpio_num_t>(pin);
    gpio_set_direction(latch, GPIO_MODE_OUTPUT);
    gpio_set_level(latch, 0);
    gpio_hold_en(latch);
  }
#else
  // Keep configured power latches asserted through deep sleep. The SDK isolates
  // GPIO pads before sleeping, so an unheld latch can float LOW once external
  // power is removed and turn a fast wake into a cold boot. This is deliberately
  // complementary to the C3 path above, where the battery latch must go LOW.
  for (const int8_t pin : {BoardConfig::ACTIVE.power.latch0, BoardConfig::ACTIVE.power.latch1}) {
    if (pin < 0 || BoardConfig::latchConflictsWithBus(pin)) continue;
    const auto latch = static_cast<gpio_num_t>(pin);
    gpio_hold_dis(latch);
    gpio_set_direction(latch, GPIO_MODE_OUTPUT);
    gpio_set_level(latch, 1);
    gpio_hold_en(latch);
  }
#endif

  // Cut the gated peripheral rails (touch/SD/EPD on boards like the Sticky) and
  // hold the enables off through deep sleep — otherwise the GT911 and SD card
  // stay powered all through "off" and drain the battery. No-op on boards with
  // no switched rails (X4/X3). Trade-off: no touch-to-wake; wake is the power
  // button. Must run after display.deepSleep() so the panel controller gets its
  // deep-sleep command while its rail is still up (enterDeepSleep() in main.cpp
  // guarantees that ordering).
  setSleepStep("rails off");
  freeink::PowerManager::powerDownRailsForSleep();

  // The SDK convenience helper currently isolates every GPIO after arming the
  // wake source. On the ESP32-C3 that overwrites the power pin's sleep input
  // configuration, so short presses can be missed. Isolate first, then restore
  // and arm the board-configured power pin immediately before sleeping.
  setSleepStep("power button release");
  freeink::PowerManager::waitForPowerButtonRelease();
  const int8_t stat = BoardConfig::ACTIVE.batteryChargeStatus;
  const int statLevel = stat >= 0 ? gpio_get_level(static_cast<gpio_num_t>(stat)) : 0;
  esp_sleep_config_gpio_isolate();
  // Auto light sleep (CONFIG_PM_ENABLE builds) enables a timer wakeup before
  // every idle nap and never disables it again. A timer wakeup left armed here
  // fires within milliseconds of deep sleep entry and comes back as a
  // DEEPSLEEP/TIMER reset instead of staying asleep. Only the power button
  // should wake the device, so clear every source before arming it.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  freeink::PowerManager::armPowerButtonWakeup();
#if SOC_PM_SUPPORT_EXT1_WAKEUP
  // X4 Pro: Up and Down wake too, so holding both while asleep shows the clock
  // (HalGPIO::checkPageKeyWake). Same active-low pull-up as Power, so
  // no extra sleep current until a key is pressed. A page key still down after
  // 2 s (pressed under something) leaves both out: alone it would wake again at
  // once, and with the other key it would still complete the chord.
  const auto& in = BoardConfig::ACTIVE.input;
  if (BoardConfig::isX4Pro() && in.power >= 0 && !in.powerActiveHigh && in.up >= 0 && in.down >= 0) {
    const unsigned long releaseStart = millis();
    while ((digitalRead(in.up) == LOW || digitalRead(in.down) == LOW) && millis() - releaseStart < 2000) {
      delay(10);
    }
    // RTC_DATA_ATTR survives deep sleep and resets on a full boot. Charger and stray-key wakes sleep again
    // from setup() and keep the lockout; a sleep after a full app wake (fullWake) clears it.
    static RTC_DATA_ATTR bool pageKeysLockedOut = false;
    if (fullWake) pageKeysLockedOut = false;
    uint64_t mask = 1ULL << in.power;
    if (digitalRead(in.up) == LOW || digitalRead(in.down) == LOW) {
      pageKeysLockedOut = true;
      LOG_INF("PWR", "Up/Down held at sleep: page-key wake locked out until the next full wake");
    }
    if (!pageKeysLockedOut) {
      pinMode(in.up, INPUT_PULLUP);
      pinMode(in.down, INPUT_PULLUP);
      mask |= (1ULL << in.up) | (1ULL << in.down);
    }
    esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
#endif
#if SOC_PM_SUPPORT_EXT0_WAKEUP
  // The opposite of STAT's level now, so a charge start or stop wakes the device.
  // An unpowered charger (cable out) leaves STAT floating: pull it toward "not
  // charging". ext0 keeps the RTC peripherals powered through sleep.
  if (wakeOnChargeChange && stat >= 0 && rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(stat))) {
    const auto pin = static_cast<gpio_num_t>(stat);
    if (esp_sleep_enable_ext0_wakeup(pin, !statLevel) == ESP_OK) {
      const bool activeHigh = BoardConfig::ACTIVE.batteryChargeStatusActiveHigh;
      activeHigh ? rtc_gpio_pullup_dis(pin) : rtc_gpio_pulldown_dis(pin);
      activeHigh ? rtc_gpio_pulldown_en(pin) : rtc_gpio_pullup_en(pin);
    }
  }
#endif
  gpio_deep_sleep_hold_en();
  // The ROM otherwise prints its boot banner at 115200 baud on every deep-sleep
  // wake before the bootloader runs. setup() logs the reset and wake causes.
  esp_deep_sleep_disable_rom_logging();
  setSleepStep("deep sleep start");
  esp_deep_sleep_start();
}

uint16_t HalPowerManager::getBatteryPercentage() const {
  static const BatteryMonitor battery;
  if (BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0) {
    const unsigned long now = millis();
    if (_batteryLastPollMs != 0 && (now - _batteryLastPollMs) < BATTERY_POLL_MS) {
      return _batteryCachedPercent;
    }

    _batteryLastPollMs = now;
    uint16_t fine = 0;
    if (!battery.readPercentage256Checked(fine)) {
      return _batteryCachedPercent;
    }
    _batteryCached256 = fine;
    _batteryCachedPercent = fine >> 8;
    return _batteryCachedPercent;
  }

  // smooth the battery %.
  if (_batteryCachedPercent == 0) {
    _batteryCachedPercent = 10 * battery.readPercentage();
  } else {
    _batteryCachedPercent = (_batteryCachedPercent * 9 + battery.readPercentage() * 10) / 10;
  }
  return _batteryCachedPercent / 10;
}

uint16_t HalPowerManager::getBatteryPercent256() const {
  const uint16_t percent = getBatteryPercentage();
  return BoardConfig::ACTIVE.batteryGauge.gaugeAddr != 0 ? _batteryCached256 : static_cast<uint16_t>(percent * 256);
}

#if CROSSDINK_BATTERY_DIAG_LOG
bool HalPowerManager::getBatteryDiagnostics(BatteryDiagnostics& out) const {
  // Function-local like getBatteryPercentage()'s: BoardConfig::ACTIVE is only
  // resolved once HalGPIO::begin() has run the X3/X4 probe, so a file-scope
  // instance could be constructed against an unresolved profile.
  static const BatteryMonitor battery;
  const BatteryMonitor::Status status = battery.readStatus();
  if (!status.supported) {
    LOG_ERR("PWR", "Battery diagnostics unsupported on this board");
    return false;
  }
  out.soc = status.percentage;
  out.millivolts = status.millivolts;
  out.charging = status.charging;
  out.socKnown = status.percentageKnown;
  out.millivoltsKnown = status.millivoltsKnown;
  out.chargingKnown = status.chargingKnown;
  return true;
}
#endif

HalPowerManager::Lock::Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  ++powerManager.lockCount;
  xSemaphoreGive(powerManager.modeMutex);
  // Immediately restore normal CPU frequency if currently in low-power mode
  powerManager.setPowerSaving(false);
}

HalPowerManager::Lock::~Lock() {
  xSemaphoreTake(powerManager.modeMutex, portMAX_DELAY);
  --powerManager.lockCount;
  xSemaphoreGive(powerManager.modeMutex);
}
