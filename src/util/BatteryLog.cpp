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
#include <esp_rom_crc.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstddef>
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
constexpr uint32_t kStatsMagic = 0x42415454;  // "BATT", CrossDink-only
constexpr uint16_t kStatsVersion = 1;         // bump on any Stats layout change
constexpr uint32_t kMaxFileBytes = 256 * 1024;
constexpr char kHeader[] = "epoch_utc,local_time,uptime_ms,pct,mv,chg,usb,temp_c,light_pct,event,detail\n";
constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kClockMs = 60 * 1000;
constexpr uint32_t kFlushIdleMs = 2000;
constexpr uint16_t kLowPct = 5;
constexpr uint32_t kLowFlushBytes = 4 * 1024;
constexpr uint32_t kMaxSleepEvents = sizeof(Stats::sleepEvents) / sizeof(Stats::SleepEvent);
constexpr uint16_t kFullPct = 95;  // charging stopped at or above this, cable in: "charged"
// A USB or charger change is logged once it has held this long: a loose plug
// flapped both every ~3 s for 2 min (20261001T190824Z-0910d7fc-battery.csv L389-415).
constexpr uint32_t kDebounceMs = 5000;
constexpr uint32_t kSlowMaxAgeMs = 5000;  // mV/temp reused this long (light slider bursts)

using Ring = PsramRing<kRingBytes>;  // aux: bytes already on SD
EXT_RAM_NOINIT_ATTR Ring ring;
RTC_NOINIT_ATTR Stats rtcStats;

portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
bool ringReady = false;

// Main-loop readings; every row reuses the latest.
struct Reading {
  uint16_t pct = 0;
  uint16_t pct256 = 0;  // same read in 1/256 % (CW2017 fraction); 0 = log the whole percent
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
uint32_t slowAtMs = 0;            // last mV/temp read
uint32_t usbSinceMs = 0;          // raw USB has differed from reading.usb since; 0 = same
uint32_t chgSinceMs = 0;          // same for charging
TaskHandle_t mainTask = nullptr;  // gauge I2C only from here (touch polls the bus too)
uint32_t lastTickMs = 0;
uint32_t carryMs = 0;
bool bootFlushPending = false;
bool wifiOn = false;

uint32_t statsCrc() { return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&rtcStats), offsetof(Stats, crc)); }

// After every change to rtcStats.
void seal() { rtcStats.crc = statsCrc(); }

// Checked once per boot. RTC bytes are never trusted after a power-on (they are
// noise), or when another firmware (CrossPoint, crossink, an older build) left
// something else there: magic and CRC must both match, else start from zero.
bool statsChecked = false;
Stats& st() {
  if (!statsChecked) {
    statsChecked = true;
    if (rtcStats.magic != kStatsMagic || rtcStats.version != kStatsVersion || rtcStats.size != sizeof(Stats) ||
        rtcStats.crc != statsCrc() || esp_reset_reason() == ESP_RST_POWERON) {
      rtcStats = Stats{kStatsMagic, kStatsVersion, sizeof(Stats)};
      seal();
    }
  }
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

// A raw state that differs from the logged one becomes it after kDebounceMs.
bool settle(const bool raw, bool& logged, uint32_t& sinceMs, const uint32_t nowMs) {
  if (raw == logged) {
    sinceMs = 0;
    return false;
  }
  if (sinceMs == 0) sinceMs = nowMs | 1;
  if (nowMs - sinceMs < kDebounceMs) return false;
  logged = raw;
  sinceMs = 0;
  return true;
}

// Cheap reads, every poll. debounce: USB and charging go through settle();
// otherwise (boot, sleep) they are taken as read.
void readQuick(const bool debounce = false) {
  Reading r = reading;
  r.pct256 = powerManager.getBatteryPercent256();
  r.pct = r.pct256 >> 8;
  const bool chg = monitor().isCharging();
  const bool usb = gpio.isUsbConnectedCached();
  if (debounce) {
    const uint32_t nowMs = millis();
    settle(chg, r.chg, chgSinceMs, nowMs);
    settle(usb, r.usb, usbSinceMs, nowMs);
  } else {
    r.chg = chg;
    r.usb = usb;
    chgSinceMs = usbSinceMs = 0;
  }
  setReading(r);
}

uint8_t userLight() { return Frontlight.present() && Frontlight.isOn() ? Frontlight.brightness() : 0; }

// Gauge I2C reads, only for rows the main loop writes.
void readSlow() {
  Reading r = reading;
  r.mv = monitor().readMillivolts();
  r.tempKnown = monitor().readTemperatureDeciC(r.tempDeciC);
  setReading(r);
  slowAtMs = millis() | 1;
}

void writeRowAt(const uint32_t epoch, const Reading& r, const char* name, const char* detail) {
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
  // Two decimals when the gauge gave a fraction: "71.43".
  char pct[8];
  if (r.pct256 != 0) {
    snprintf(pct, sizeof(pct), "%u.%02u", r.pct256 >> 8, (r.pct256 & 0xFF) * 100 / 256);
  } else {
    snprintf(pct, sizeof(pct), "%u", r.pct);
  }
  char row[160];
  int n = snprintf(row, sizeof(row), "%lu,%s,%lu,%s,%u,%u,%u,%s,%u,%s,%s\n", static_cast<unsigned long>(epoch), local,
                   static_cast<unsigned long>(millis()), pct, r.mv, r.chg ? 1u : 0u, r.usb ? 1u : 0u, temp, r.light,
                   name, detail ? detail : "");
  if (n <= 0) return;
  if (n >= static_cast<int>(sizeof(row))) {
    n = sizeof(row) - 1;
    row[n - 1] = '\n';
  }
  append(row, static_cast<uint32_t>(n));
  LOG_INF("BAT", "%.*s", n - 1, row);
}

void writeRow(const char* name, const char* detail) {
  // Fresh mV/temp on every row the main loop writes; rows from other tasks
  // (firmware flasher) reuse the last read.
  if (xTaskGetCurrentTaskHandle() == mainTask && (slowAtMs == 0 || millis() - slowAtMs >= kSlowMaxAgeMs)) readSlow();
  portENTER_CRITICAL_SAFE(&ringMux);
  const Reading r = reading;
  portEXIT_CRITICAL_SAFE(&ringMux);
  writeRowAt(epochNow(), r, name, detail);
}

// Sleep and restarts end Wi-Fi without the poll seeing it: say so, so every
// wifi_on has its wifi_off. No gauge read here (a restart may be mid-I2C).
void wifiEnded(const char* why) {
  if (!wifiOn) return;
  wifiOn = false;
  portENTER_CRITICAL_SAFE(&ringMux);
  const Reading r = reading;
  portEXIT_CRITICAL_SAFE(&ringMux);
  writeRowAt(epochNow(), r, "wifi_off", why);
}

}  // namespace

// The row stays in the PSRAM ring; the next boot flushes it.
void onRestart() { wifiEnded("restart"); }

namespace {

// Adds the sleep since sleepEpoch to the counters and restarts it at epoch.
void settleSleep(Stats& s, const uint32_t epoch, const uint16_t pct, const bool usbNow) {
  if (s.sleepEpoch != 0 && epoch > s.sleepEpoch) {
    const uint32_t slept = epoch - s.sleepEpoch;
    s.asleepS += slept;
    if (!s.sleepUsb && !usbNow) {
      s.battAsleepS += slept;
      if (pct < s.sleepPct) s.dropAsleepPct += s.sleepPct - pct;
    }
  }
  s.sleepEpoch = epoch;
  s.sleepPct = pct;
  s.sleepUsb = usbNow;
}

// Charging stopped: the "since last charged" counters start over.
void markCharged(Stats& s, const uint32_t epoch, const uint16_t pct) {
  s.chargedEpoch = epoch;
  s.chargedPct = pct;
  s.battAwakeS = s.battAsleepS = s.dropAwakePct = s.dropAsleepPct = 0;
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
  mainTask = xTaskGetCurrentTaskHandle();
  lastTickMs = millis();
  readClock();
  readQuick();
  readSlow();
  wifiOn = WiFi.getMode() != WIFI_OFF;
  reading.light = userLight();
  Stats& s = st();
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool wake = cause != ESP_SLEEP_WAKEUP_UNDEFINED;
  if (wake) {
    s.wakes++;
    const uint32_t epoch = epochNow();
    if (epoch != 0) settleSleep(s, epoch, reading.pct, reading.usb);
  } else {
    s.boots++;
  }
  s.sleepEpoch = 0;
  // Charge starts/stops that woke the device briefly while it slept. A change
  // undone within kDebounceMs (a flapping STAT line) is dropped with its undo.
  const uint32_t events = std::min(s.sleepEventCount, kMaxSleepEvents);
  for (uint32_t i = 0; i < events; ++i) {
    const Stats::SleepEvent& e = s.sleepEvents[i];
    if (i + 1 < events && s.sleepEvents[i + 1].chg != e.chg &&
        s.sleepEvents[i + 1].epoch - e.epoch < kDebounceMs / 1000) {
      ++i;
      continue;
    }
    Reading r = reading;
    r.pct = e.pct;
    r.pct256 = 0;
    r.mv = e.mv;
    r.chg = r.usb = e.chg;
    r.tempKnown = false;
    r.light = 0;
    writeRowAt(e.epoch, r, e.chg ? "chg_on" : e.pct >= kFullPct ? "charged" : "chg_off", "asleep");
  }
  s.sleepEventCount = 0;
  char detail[96];
  int n =
      snprintf(detail, sizeof(detail), "reset=%s wake=%s", resetReasonName(esp_reset_reason()), wakeupCauseName(cause));
  if (s.pendingFalseWakes != 0 && n > 0 && n < static_cast<int>(sizeof(detail))) {
    snprintf(detail + n, sizeof(detail) - n, " false_wakes=%lu awake_ms=%lu",
             static_cast<unsigned long>(s.pendingFalseWakes), static_cast<unsigned long>(s.pendingFalseWakeMs));
  }
  s.falseWakes += s.pendingFalseWakes;
  s.pendingFalseWakes = s.pendingFalseWakeMs = 0;
  seal();
  writeRow(wake ? "wake" : "boot", detail);
  powerManager.wakeOnChargeChange = true;
  bootFlushPending = true;  // also saves rows a restart or crash left in the ring
}

void onSleep(const char* why) {
  tick(reading.usb);
  readQuick();
  readSlow();
  wifiEnded("sleep");
  Stats& s = st();
  s.sleepEpoch = epochNow();
  s.sleepPct = reading.pct;
  s.sleepUsb = reading.usb;
  seal();
  writeRow("sleep", why);
  flush();
}

void poll(const uint32_t idleMs) {
  const uint32_t nowMs = millis();
  if (nowMs - lastPollMs < kPollMs) return;
  lastPollMs = nowMs;
  if (nowMs - epochBaseMs >= kClockMs) readClock();

  const Reading before = reading;
  readQuick(/*debounce=*/true);
  tick(before.usb);
  Stats& s = st();
  if (reading.usb != before.usb) writeRow(reading.usb ? "usb_in" : "usb_out", nullptr);
  if (reading.chg != before.chg) {
    if (!reading.chg) markCharged(s, epochNow(), reading.pct);
    // "charged": charging stopped near full with the cable in and settled.
    const bool full = reading.usb && usbSinceMs == 0 && reading.pct >= kFullPct;
    writeRow(reading.chg ? "chg_on" : full ? "charged" : "chg_off", nullptr);
  }
  if (reading.pct != before.pct) {
    if (reading.pct < before.pct && !reading.usb) s.dropAwakePct += before.pct - reading.pct;
    writeRow("pct", nullptr);
  }
  const bool wifi = WiFi.getMode() != WIFI_OFF;
  if (wifi != wifiOn) {
    wifiOn = wifi;
    writeRow(wifi ? "wifi_on" : "wifi_off", nullptr);
  }
  seal();
  if (!ringReady || idleMs < kFlushIdleMs) return;
  const uint32_t pending = ring.head - ring.aux;
  // Low battery: out in 4 KB lots (not row by row) before a brownout can take the ring.
  const bool low = !reading.usb && reading.pct <= kLowPct && pending >= kLowFlushBytes;
  if (pending != 0 && (bootFlushPending || low || pending >= kRingBytes / 4 * 3) && flush()) {
    bootFlushPending = false;
  }
}

void onChargeWake() {
  Stats& s = st();
  readClock();
  Reading r = reading;
  r.pct256 = powerManager.getBatteryPercent256();
  r.pct = r.pct256 >> 8;
  r.chg = monitor().isCharging();
  r.mv = monitor().readMillivolts();
  setReading(r);
  const uint32_t epoch = epochNow();
  if (s.sleepEventCount < kMaxSleepEvents) {
    s.sleepEvents[s.sleepEventCount++] = {epoch, r.mv, static_cast<uint8_t>(std::min<uint16_t>(r.pct, 100)), r.chg};
  }
  if (epoch != 0) {
    settleSleep(s, epoch, r.pct, r.chg || r.pct >= kFullPct);
    if (!r.chg) markCharged(s, epoch, r.pct);
  }
  seal();
  // A flapping STAT line (charger fault blink) stops waking the device once the list is full.
  powerManager.wakeOnChargeChange = s.sleepEventCount < kMaxSleepEvents;
  LOG_INF("BAT", "Charge wake: %s at %u%%", r.chg ? "charging" : "stopped", r.pct);
}

void noteFalseWake() {
  Stats& s = st();
  s.pendingFalseWakes++;
  s.pendingFalseWakeMs += millis();
  seal();
  powerManager.wakeOnChargeChange = s.sleepEventCount < kMaxSleepEvents;
}

void event(const char* name, const char* detail) { writeRow(name, detail); }

void lightChanged(const bool timedOut) {
  Reading r = reading;
  r.light = timedOut ? 0 : userLight();
  if (r.light == reading.light) return;
  setReading(r);
  writeRow("light", nullptr);
}

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
    Storage.remove(LOG_PATHS[LOG_FILES - 1]);
    for (int i = LOG_FILES - 1; i > 0; --i) {
      if (Storage.exists(LOG_PATHS[i - 1]) && !Storage.rename(LOG_PATHS[i - 1], LOG_PATHS[i])) {
        LOG_ERR("BAT", "Failed to rotate %s", LOG_PATHS[i - 1]);
        return false;
      }
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

void forEachPending(void (*sink)(void*, const char*, uint32_t), void* ctx) {
  if (!ensureRing()) return;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  uint32_t at = ring.aux;
  portEXIT_CRITICAL_SAFE(&ringMux);
  if (head - at > kRingBytes) at = head - kRingBytes;
  char chunk[256];
  while (at != head) {
    const uint32_t n = std::min(head - at, static_cast<uint32_t>(sizeof(chunk)));
    ring.copyOut(at, chunk, n);
    sink(ctx, chunk, n);
    at += n;
  }
}

const Stats& stats() { return st(); }

uint32_t nowEpoch() { return epochNow(); }

void reset() {
  rtcStats = Stats{kStatsMagic, kStatsVersion, sizeof(Stats)};
  seal();
  auto& counts = HalDisplay::refreshCounts();
  std::fill(std::begin(counts.n), std::end(counts.n), 0u);
  LOG_INF("BAT", "Stats reset");
}

}  // namespace BatteryLog

#endif  // CROSSDINK_GOODIES && !SIMULATOR
