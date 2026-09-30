#include "BatteryLog.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <BatteryMonitor.h>
#include <CrossDinkHalFrontlight.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PsramRing.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_psram.h>
#include <esp_sleep.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "BootReason.h"
#include "LocalClock.h"

#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error "BatteryLog needs CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y"
#endif

namespace BatteryLog {
namespace {
constexpr uint32_t kRingBytes = 64 * 1024;
constexpr uint32_t kRingMagicA = 0x4241544C;  // "BATL"
constexpr uint32_t kRingMagicB = 0xC0DE0930;
constexpr uint32_t kStatsMagic = 0x42415453;  // "BATS"
constexpr uint32_t kMaxFileBytes = 256 * 1024;
constexpr char kOldPath[] = "/debug/logs/battery.1.csv";
constexpr char kHeader[] = "epoch_utc,local_time,uptime_ms,pct,mv,chg,usb,temp_c,light_pct,event,detail\n";
constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kClockMs = 60 * 1000;
constexpr uint32_t kFlushIdleMs = 2000;
constexpr uint32_t kLightSettleMs = 2000;
constexpr uint16_t kLowPct = 5;

using Ring = PsramRing<kRingBytes>;  // aux: bytes already on SD
EXT_RAM_NOINIT_ATTR Ring ring;
RTC_NOINIT_ATTR Stats rtcStats;

portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
bool ringReady = false;

// Main-loop readings; every row reuses the latest.
struct Reading {
  uint16_t pct = 0;
  uint16_t mv = 0;
  int16_t tempDeciC = 0;
  bool tempKnown = false;
  bool chg = false;
  bool usb = false;
  uint8_t light = 0;
};
Reading reading;
uint32_t epochBase = 0;  // 0: RTC not set
uint32_t epochBaseMs = 0;
int32_t localOffsetS = 0;

uint32_t lastPollMs = 0;
uint32_t lastTickMs = 0;
uint32_t carryMs = 0;
bool bootFlushPending = false;
bool wifiOn = false;
uint8_t loggedLight = 0;
uint8_t pendingLight = 0;
uint32_t pendingLightMs = 0;

Stats& st() {
  if (rtcStats.magic != kStatsMagic) rtcStats = Stats{kStatsMagic};
  return rtcStats;
}

// PSRAM is mapped after global constructors, so the ring is set up on first use.
bool ensureRing() {
  if (ringReady) return true;
  if (!esp_psram_is_initialized()) return false;
  portENTER_CRITICAL_SAFE(&ringMux);
  if (!ringReady) {
    if (!ring.adopt(kRingMagicA, kRingMagicB) || ring.aux > ring.head) ring.aux = ring.head;
    ringReady = true;
  }
  portEXIT_CRITICAL_SAFE(&ringMux);
  ring.writeBackHeader();
  return true;
}

void append(const char* text, const uint32_t len) {
  if (!ensureRing()) return;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t startHead = ring.head;
  ring.write(text, len);
  portEXIT_CRITICAL_SAFE(&ringMux);
  ring.writeBack(startHead, len);
}

uint32_t toEpoch(uint32_t y, const uint32_t mo, const uint32_t d, const uint32_t h, const uint32_t mi,
                 const uint32_t s) {
  y -= mo <= 2;  // days-from-civil (Howard Hinnant), years >= 2000 only
  const uint32_t era = y / 400;
  const uint32_t yoe = y - era * 400;
  const uint32_t doy = (153 * (mo > 2 ? mo - 3 : mo + 9) + 2) / 5 + d - 1;
  const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const uint32_t days = era * 146097 + doe - 719468;
  return days * 86400 + h * 3600 + mi * 60 + s;
}

void readClock() {
  epochBaseMs = millis();
  uint16_t y = 0;
  uint8_t mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (!halClock.getDateTime(y, mo, d, h, mi, s) || y < 2024) {
    epochBase = 0;
    return;
  }
  epochBase = toEpoch(y, mo, d, h, mi, s);
  localOffsetS = (static_cast<int32_t>(LocalClock::currentOffsetQ()) - 48) * 15 * 60;
}

uint32_t epochNow() { return epochBase != 0 ? epochBase + (millis() - epochBaseMs) / 1000 : 0; }

const BatteryMonitor& monitor() {
  static const BatteryMonitor m;  // built on first use, after BoardConfig::ACTIVE is set
  return m;
}

// Only the main loop writes `reading`; rows from other tasks copy it under ringMux.
void setReading(const Reading& r) {
  portENTER_CRITICAL_SAFE(&ringMux);
  reading = r;
  portEXIT_CRITICAL_SAFE(&ringMux);
}

// Cheap reads, every poll.
void readQuick() {
  Reading r = reading;
  r.pct = powerManager.getBatteryPercentage();
  r.chg = monitor().isCharging();
  r.usb = gpio.isUsbConnectedCached();
  r.light = Frontlight.present() && Frontlight.isOn()
                ? static_cast<uint8_t>(Frontlight.brightness() * Frontlight.idleDimPercent() / 100)
                : 0;
  setReading(r);
}

// Gauge I2C reads, only for rows the main loop writes.
void readSlow() {
  Reading r = reading;
  r.mv = monitor().readMillivolts();
  r.tempKnown = monitor().readTemperatureDeciC(r.tempDeciC);
  setReading(r);
}

void writeRow(const char* name, const char* detail) {
  portENTER_CRITICAL_SAFE(&ringMux);
  const Reading r = reading;
  portEXIT_CRITICAL_SAFE(&ringMux);
  const uint32_t epoch = epochNow();
  char local[20] = "";
  if (epoch != 0) {
    const time_t t = static_cast<time_t>(epoch) + localOffsetS;
    tm parts{};
    gmtime_r(&t, &parts);
    strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &parts);
  }
  char temp[8] = "";
  if (r.tempKnown) {
    const int t = std::abs(static_cast<int>(r.tempDeciC));
    snprintf(temp, sizeof(temp), "%s%d.%d", r.tempDeciC < 0 ? "-" : "", t / 10, t % 10);
  }
  char row[160];
  int n = snprintf(row, sizeof(row), "%lu,%s,%lu,%u,%u,%u,%u,%s,%u,%s,%s\n", static_cast<unsigned long>(epoch), local,
                   static_cast<unsigned long>(millis()), r.pct, r.mv, r.chg ? 1u : 0u, r.usb ? 1u : 0u, temp,
                   r.light, name, detail ? detail : "");
  if (n <= 0) return;
  if (n >= static_cast<int>(sizeof(row))) {
    n = sizeof(row) - 1;
    row[n - 1] = '\n';
  }
  append(row, static_cast<uint32_t>(n));
  LOG_INF("BAT", "%.*s", n - 1, row);
}

// Adds the time since the last tick to the awake counters.
void tick(const bool onUsb) {
  const uint32_t nowMs = millis();
  carryMs += nowMs - lastTickMs;
  lastTickMs = nowMs;
  const uint32_t s = carryMs / 1000;
  carryMs %= 1000;
  st().awakeS += s;
  if (!onUsb) st().battAwakeS += s;
}
}  // namespace

void onBoot() {
  lastTickMs = millis();
  readClock();
  readQuick();
  readSlow();
  wifiOn = WiFi.getMode() != WIFI_OFF;
  loggedLight = pendingLight = reading.light;
  Stats& s = st();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool wake = cause != ESP_SLEEP_WAKEUP_UNDEFINED;
  if (wake) {
    s.wakes++;
    const uint32_t epoch = epochNow();
    if (s.sleepEpoch != 0 && epoch > s.sleepEpoch) {
      const uint32_t slept = epoch - s.sleepEpoch;
      s.asleepS += slept;
      if (!s.sleepUsb && !reading.usb) {
        s.battAsleepS += slept;
        if (reading.pct < s.sleepPct) s.dropAsleepPct += s.sleepPct - reading.pct;
      }
    }
  } else {
    s.boots++;
  }
  s.sleepEpoch = 0;
  char detail[64];
  snprintf(detail, sizeof(detail), "reset=%s wake=%s", resetReasonName(esp_reset_reason()), wakeupCauseName(cause));
  writeRow(wake ? "wake" : "boot", detail);
  bootFlushPending = true;  // also saves rows a restart or crash left in the ring
}

void onSleep(const char* why) {
  tick(reading.usb);
  readQuick();
  readSlow();
  Stats& s = st();
  s.sleepEpoch = epochNow();
  s.sleepPct = reading.pct;
  s.sleepUsb = reading.usb;
  writeRow("sleep", why);
  flush();
}

void poll(const uint32_t idleMs) {
  const uint32_t nowMs = millis();
  if (nowMs - lastPollMs < kPollMs) return;
  lastPollMs = nowMs;
  if (nowMs - epochBaseMs >= kClockMs) readClock();

  const Reading before = reading;
  readQuick();
  tick(before.usb);
  Stats& s = st();
  if (reading.usb != before.usb) {
    readSlow();
    if (!reading.usb) {
      s.unplugEpoch = epochNow();
      s.unplugPct = reading.pct;
      s.battAwakeS = s.battAsleepS = s.dropAwakePct = s.dropAsleepPct = 0;
    }
    writeRow(reading.usb ? "usb_in" : "usb_out", nullptr);
  }
  if (reading.chg != before.chg) {
    readSlow();
    writeRow(reading.chg ? "chg_on" : reading.usb ? "charged" : "chg_off", nullptr);
  }
  if (reading.pct != before.pct) {
    if (reading.pct < before.pct && !reading.usb) s.dropAwakePct += before.pct - reading.pct;
    readSlow();
    writeRow("pct", nullptr);
  }
  const bool wifi = WiFi.getMode() != WIFI_OFF;
  if (wifi != wifiOn) {
    wifiOn = wifi;
    writeRow(wifi ? "wifi_on" : "wifi_off", nullptr);
  }
  // Logged once it holds for 2 s: a slider drag or a transfer pulse is one row, not dozens.
  if (reading.light != pendingLight) {
    pendingLight = reading.light;
    pendingLightMs = nowMs;
  } else if (pendingLight != loggedLight && nowMs - pendingLightMs >= kLightSettleMs) {
    loggedLight = pendingLight;
    writeRow("light", nullptr);
  }

  if (!ringReady || idleMs < kFlushIdleMs) return;
  const uint32_t pending = ring.head - ring.aux;
  const bool low = !reading.usb && reading.pct <= kLowPct;
  if (pending != 0 && (bootFlushPending || low || pending >= kRingBytes / 4 * 3) && flush()) {
    bootFlushPending = false;
  }
}

void event(const char* name, const char* detail) { writeRow(name, detail); }

bool flush() {
  if (!ensureRing() || !Storage.ready()) return false;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  uint32_t from = ring.aux;
  portEXIT_CRITICAL_SAFE(&ringMux);
  if (from == head) return true;
  if (head - from > kRingBytes) {
    LOG_ERR("BAT", "ring overran: %lu bytes lost", static_cast<unsigned long>(head - from - kRingBytes));
    from = head - kRingBytes;
  }
  Storage.ensureDirectoryExists("/debug/logs");
  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("BAT", "Failed to open %s", LOG_PATH);
    return false;
  }
  if (file.fileSize() >= kMaxFileBytes) {
    file.close();
    Storage.remove(kOldPath);
    if (!Storage.rename(LOG_PATH, kOldPath)) {
      LOG_ERR("BAT", "Failed to rotate %s", LOG_PATH);
      return false;
    }
    return flush();  // the fresh file is empty, so this recurses once
  }
  bool ok = true;
  if (file.fileSize() == 0) ok = file.write(kHeader, sizeof(kHeader) - 1) == sizeof(kHeader) - 1;
  // Through DRAM: the SD driver is not handed PSRAM buffers.
  char chunk[256];
  for (uint32_t at = from; ok && at != head;) {
    const uint32_t n = std::min(head - at, static_cast<uint32_t>(sizeof(chunk)));
    ring.copyOut(at, chunk, n);
    ok = file.write(chunk, n) == n;
    at += n;
  }
  ok = file.close() && ok;
  if (!ok) {
    LOG_ERR("BAT", "Failed to append to %s", LOG_PATH);
    return false;
  }
  portENTER_CRITICAL_SAFE(&ringMux);
  ring.aux = head;
  portEXIT_CRITICAL_SAFE(&ringMux);
  ring.writeBackHeader();
  LOG_DBG("BAT", "Flushed %lu bytes to %s", static_cast<unsigned long>(head - from), LOG_PATH);
  return true;
}

const Stats& stats() { return st(); }

uint32_t nowEpoch() { return epochNow(); }

void reset() {
  rtcStats = Stats{kStatsMagic};
  auto& counts = HalDisplay::refreshCounts();
  std::fill(std::begin(counts.n), std::end(counts.n), 0u);
  LOG_INF("BAT", "Stats reset");
}

}  // namespace BatteryLog

#endif  // CROSSDINK_GOODIES && !SIMULATOR
