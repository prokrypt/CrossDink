#include "PowerCounters.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_pm.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <cstddef>

namespace PowerCounters {
namespace {
constexpr uint32_t kMagic = 0x50574354;  // "PWCT"
constexpr uint16_t kVersion = 1;         // bump on any Rtc or Totals layout change
constexpr int kCores = 2;

// RTC slow memory: survives deep sleep and restarts, noise after power-on.
struct Rtc {
  uint32_t magic;
  uint16_t version;
  uint16_t size;  // sizeof(Rtc)
  uint16_t gen;
  uint8_t finalSave;  // written by the sleep or restart path, so `t` is exact
  uint8_t chargeWake;
  uint32_t sleepEpoch;  // UTC seconds deep sleep began, 0 = none open
  uint8_t sleepExt0;
  uint8_t reserved[3];
  Totals t;
  uint32_t crc;  // over everything above
};
RTC_NOINIT_ATTR Rtc rtc;

uint32_t rtcCrc() { return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&rtc), offsetof(Rtc, crc)); }
void seal() { rtc.crc = rtcCrc(); }
bool rtcValid() {
  return rtc.magic == kMagic && rtc.version == kVersion && rtc.size == sizeof(Rtc) && rtc.crc == rtcCrc();
}

// Counts since this boot. BSS, so zero before any hook can fire. Guarded by
// mux: hooks run on the render task, storage callers, the Wi-Fi event task
// and (light sleep) the idle task inside the PM switch lock.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Totals boot;
Totals base;  // restored by begin(); main loop only
uint16_t gen = 0;
uint32_t awakeZeroMs = 0;  // millis() that counts as zero awake time this boot (moved by reset())

// Open segments, timed with esp_timer (us since boot; runs through light sleep).
bool maxClockHeld = false;
int64_t maxClockSince = 0;
uint16_t dutyNow = 0;
int64_t dutySince = 0;
bool panelBusyOpen = false;
int64_t panelBusySince = 0;
PanelKind panelBusyKind = PANEL_FAST;
PanelKind lastKind = PANEL_FAST;
bool boosterOn = false;
int64_t boosterSince = 0;

// Per-core busy time (tick, main loop only).
uint32_t prevIdle[kCores] = {};
int64_t prevWallUs = 0;

uint64_t msOf(const int64_t us) { return us > 0 ? static_cast<uint64_t>(us) / 1000 : 0; }

void add(Totals& a, const Totals& b) {
  a.awakeMs += b.awakeMs;
  for (int i = 0; i < 2; ++i) a.asleepS[i] += b.asleepS[i];
  a.lightSleepUs += b.lightSleepUs;
  a.maxClockUs += b.maxClockUs;
  for (int i = 0; i < kCores; ++i) a.busyUs[i] += b.busyUs[i];
  for (int i = 0; i < RADIO_STATES; ++i) a.wifiMs[i] += b.wifiMs[i];
  a.wifiScans += b.wifiScans;
  a.wifiConnects += b.wifiConnects;
  a.ipTxPackets += b.ipTxPackets;
  a.ipRxPackets += b.ipRxPackets;
  a.lightDutyMs += b.lightDutyMs;
  for (int i = 0; i < PANEL_KINDS; ++i) a.panelBusyMs[i] += b.panelBusyMs[i];
  a.boosterMs += b.boosterMs;
  a.sdReadBytes += b.sdReadBytes;
  a.sdWriteBytes += b.sdWriteBytes;
  a.sdUs += b.sdUs;
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// Idle task, PM switch lock held, cache on. sleptUs <= 0: the sleep was skipped.
esp_err_t IRAM_ATTR onLightSleepExit(const int64_t sleptUs, void*) {
  if (sleptUs > 0) {
    portENTER_CRITICAL_SAFE(&mux);
    boot.lightSleepUs += static_cast<uint64_t>(sleptUs);
    portEXIT_CRITICAL_SAFE(&mux);
  }
  return ESP_OK;
}
#endif
}  // namespace

void begin(const uint32_t nowEpoch, const bool deepSleepWake) {
  static bool begun = false;
  if (begun) return;
  begun = true;
  if (rtcValid() && esp_reset_reason() != ESP_RST_POWERON) {
    base = rtc.t;
    gen = rtc.gen;
    // A crash or watchdog reset loses what counted since the last save.
    bool exact = rtc.finalSave != 0;
    if (rtc.sleepEpoch != 0) {
      if (deepSleepWake && nowEpoch > rtc.sleepEpoch) {
        base.asleepS[rtc.sleepExt0 ? 1 : 0] += nowEpoch - rtc.sleepEpoch;
      } else {
        exact = false;  // asleep for an unknown time (clock unset, or no deep sleep after all)
      }
    }
    if (!exact) ++gen;
  } else {
    base = Totals{};
    gen = static_cast<uint16_t>(esp_random());
    rtc.chargeWake = 1;
  }
  rtc.magic = kMagic;
  rtc.version = kVersion;
  rtc.size = sizeof(Rtc);
  rtc.gen = gen;
  rtc.finalSave = 0;
  rtc.sleepEpoch = 0;
  rtc.sleepExt0 = 0;
  rtc.t = base;
  seal();
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = onLightSleepExit;
  if (esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK) LOG_ERR("PWC", "light sleep hook failed: no ls time");
#endif
  LOG_INF("PWC", "Power counters gen %u%s", gen, base.awakeMs == 0 ? " (new)" : "");
}

void tick() {
#if (configGENERATE_RUN_TIME_STATS == 1) && (INCLUDE_xTaskGetIdleTaskHandle == 1)
  const int64_t nowUs = esp_timer_get_time();
  const int64_t wallUs = nowUs - prevWallUs;
  uint64_t busy[kCores] = {};
  for (int c = 0; c < kCores; ++c) {
    const uint32_t idle = static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(c));
    // The counter is 32-bit us: modular difference, valid for gaps under 71 min.
    const int64_t idleUs = static_cast<uint32_t>(idle - prevIdle[c]);
    prevIdle[c] = idle;
    if (wallUs > idleUs && wallUs < 3600LL * 1000000) busy[c] = static_cast<uint64_t>(wallUs - idleUs);
  }
  prevWallUs = nowUs;
  portENTER_CRITICAL_SAFE(&mux);
  for (int c = 0; c < kCores; ++c) boot.busyUs[c] += busy[c];
  portEXIT_CRITICAL_SAFE(&mux);
#endif
}

Totals totals() {
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&mux);
  Totals t = boot;
  if (maxClockHeld) t.maxClockUs += static_cast<uint64_t>(nowUs - maxClockSince);
  t.lightDutyMs += static_cast<uint64_t>(dutyNow) * msOf(nowUs - dutySince);
  if (panelBusyOpen) t.panelBusyMs[panelBusyKind] += msOf(nowUs - panelBusySince);
  if (boosterOn) t.boosterMs += msOf(nowUs - boosterSince);
  portEXIT_CRITICAL_SAFE(&mux);
  t.awakeMs = millis() - awakeZeroMs;
  add(t, base);
  return t;
}

uint16_t generation() { return gen; }

void save(const bool final) {
  rtc.t = totals();
  rtc.gen = gen;
  rtc.finalSave = final ? 1 : 0;
  seal();
}

void noteSleep(const uint32_t nowEpoch, const bool ext0) {
  rtc.sleepEpoch = nowEpoch;
  rtc.sleepExt0 = ext0 ? 1 : 0;
  seal();
}

void reset() {
  portENTER_CRITICAL_SAFE(&mux);
  // Open segments restart now, so nothing before the reset is counted.
  const int64_t nowUs = esp_timer_get_time();
  boot = Totals{};
  maxClockSince = dutySince = panelBusySince = boosterSince = nowUs;
  portEXIT_CRITICAL_SAFE(&mux);
  base = Totals{};
  awakeZeroMs = millis();
  ++gen;
  rtc.t = Totals{};
  rtc.gen = gen;
  rtc.finalSave = 0;
  seal();
  LOG_INF("PWC", "Power counters reset, gen %u", gen);
}

void setChargeWakeAllowed(const bool allowed) {
  if (!rtcValid()) return;
  rtc.chargeWake = allowed ? 1 : 0;
  seal();
}

bool chargeWakeAllowed() { return !rtcValid() || rtc.chargeWake != 0; }

void maxClock(const bool held) {
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&mux);
  if (held && !maxClockHeld) {
    maxClockSince = nowUs;
  } else if (!held && maxClockHeld) {
    boot.maxClockUs += static_cast<uint64_t>(nowUs - maxClockSince);
  }
  maxClockHeld = held;
  portEXIT_CRITICAL_SAFE(&mux);
}

void lightDuty(const uint16_t duty1023) {
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&mux);
  boot.lightDutyMs += static_cast<uint64_t>(dutyNow) * msOf(nowUs - dutySince);
  dutyNow = duty1023;
  dutySince = nowUs;
  portEXIT_CRITICAL_SAFE(&mux);
}

void panelKind(const PanelKind kind) {
  portENTER_CRITICAL_SAFE(&mux);
  lastKind = kind;
  portEXIT_CRITICAL_SAFE(&mux);
}

void panelBusy(const bool begin) {
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&mux);
  if (begin && !panelBusyOpen) {
    panelBusyOpen = true;
    panelBusySince = nowUs;
    panelBusyKind = lastKind;
  } else if (!begin && panelBusyOpen) {
    panelBusyOpen = false;
    boot.panelBusyMs[panelBusyKind] += msOf(nowUs - panelBusySince);
  }
  portEXIT_CRITICAL_SAFE(&mux);
}

void booster(const bool on) {
  const int64_t nowUs = esp_timer_get_time();
  portENTER_CRITICAL_SAFE(&mux);
  if (on && !boosterOn) {
    boosterSince = nowUs;
  } else if (!on && boosterOn) {
    boot.boosterMs += msOf(nowUs - boosterSince);
  }
  boosterOn = on;
  portEXIT_CRITICAL_SAFE(&mux);
}

void sdRead(const uint32_t bytes, const uint32_t us) {
  portENTER_CRITICAL_SAFE(&mux);
  boot.sdReadBytes += bytes;
  boot.sdUs += us;
  portEXIT_CRITICAL_SAFE(&mux);
}

void sdWrite(const uint32_t bytes, const uint32_t us) {
  portENTER_CRITICAL_SAFE(&mux);
  boot.sdWriteBytes += bytes;
  boot.sdUs += us;
  portEXIT_CRITICAL_SAFE(&mux);
}

void wifiMs(const RadioState state, const uint32_t ms) {
  if (state >= RADIO_STATES) return;
  portENTER_CRITICAL_SAFE(&mux);
  boot.wifiMs[state] += ms;
  portEXIT_CRITICAL_SAFE(&mux);
}

void wifiScan() {
  portENTER_CRITICAL_SAFE(&mux);
  ++boot.wifiScans;
  portEXIT_CRITICAL_SAFE(&mux);
}

void wifiConnect() {
  portENTER_CRITICAL_SAFE(&mux);
  ++boot.wifiConnects;
  portEXIT_CRITICAL_SAFE(&mux);
}

void ipPackets(const uint32_t tx, const uint32_t rx) {
  portENTER_CRITICAL_SAFE(&mux);
  boot.ipTxPackets += tx;
  boot.ipRxPackets += rx;
  portEXIT_CRITICAL_SAFE(&mux);
}

}  // namespace PowerCounters

#endif  // CROSSDINK_GOODIES && !SIMULATOR
