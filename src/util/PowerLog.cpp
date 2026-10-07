#include "PowerLog.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <FreeInkDisplay.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Knobs.h>
#include <Logging.h>
#include <PowerCounters.h>
#include <PsramRing.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_psram.h>
#include <esp_wifi.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstring>

#if CONFIG_LWIP_STATS
#include <lwip/stats.h>
#endif

#include "PowerLogRow.h"

namespace PowerLog {
namespace {
constexpr uint32_t kRingBytes = 16 * 1024;
constexpr uint32_t fnv1a(const char* s) {
  uint32_t h = 2166136261u;
  for (; *s; ++s) h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
  return h;
}
constexpr uint32_t kRingMagicA = 0x50574C47;  // "PWLG"
// Keyed to the columns: rows a build with other columns left in the ring
// (before an update's restart) are dropped, not written under this header.
constexpr uint32_t kRingMagicB = fnv1a(PowerLogRow::kHeader);
constexpr uint32_t kMaxFileBytes = 256 * 1024;
constexpr uint32_t kPollMs = 1000;
constexpr uint32_t kFlushIdleMs = 2000;
constexpr uint32_t kPanelTempMaxAgeMs = 10 * 60 * 1000;
// Battery nearly empty off USB: rows go out in 1 KB lots (about three rows) so
// the session the battery dies in is on the card, as battery.csv does.
constexpr uint16_t kLowPct = 5;
constexpr uint32_t kLowFlushBytes = 1024;

using Ring = PsramRing<kRingBytes>;  // aux: bytes already on SD
EXT_RAM_NOINIT_ATTR Ring ring;
portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
bool ringReady = false;

SampleFn sampleFn = nullptr;
bool begun = false;
bool bootFlushPending = false;
bool headerChecked = false;  // once per boot, before the first append
bool chargerSeen = false;
uint32_t lastPollMs = 0;
uint32_t lastRowMs = 0;
PowerCounters::RadioState wifiState = PowerCounters::RADIO_OFF;
uint32_t wifiSinceMs = 0;
#if CONFIG_LWIP_STATS
using IpCounter = decltype(lwip_stats.ip.xmit);  // u16 unless LWIP_STATS_LARGE: polled well inside a wrap
IpCounter prevTx = 0, prevRx = 0;
#if LWIP_IPV6
IpCounter prevTx6 = 0, prevRx6 = 0;
#endif
#endif

// Rows are formatted here, main loop only. PSRAM: snprintf and the ring copy
// are CPU work, and the SD write goes through flush()'s DRAM chunk.
EXT_RAM_NOINIT_ATTR char rowBuf[PowerLogRow::kMaxRow];

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

PowerCounters::RadioState sampleWifi() {
  const wifi_mode_t mode = WiFi.getMode();
  if (mode == WIFI_MODE_NULL) return PowerCounters::RADIO_OFF;
  if (mode & WIFI_MODE_AP) return PowerCounters::RADIO_AP;
  if (WiFi.status() != WL_CONNECTED) return PowerCounters::RADIO_UP;
  wifi_ps_type_t ps = WIFI_PS_NONE;
  if (esp_wifi_get_ps(&ps) != ESP_OK) return PowerCounters::RADIO_UP;
  return ps == WIFI_PS_NONE ? PowerCounters::RADIO_AWAKE : PowerCounters::RADIO_PS;
}

// Time in the radio state since the last sample, then the state now.
void sampleRadio(const uint32_t nowMs) {
  PowerCounters::wifiMs(wifiState, nowMs - wifiSinceMs);
  wifiSinceMs = nowMs;
  wifiState = sampleWifi();
#if CONFIG_LWIP_STATS
  const IpCounter tx = lwip_stats.ip.xmit, rx = lwip_stats.ip.recv;
  uint32_t dTx = static_cast<IpCounter>(tx - prevTx), dRx = static_cast<IpCounter>(rx - prevRx);
  prevTx = tx;
  prevRx = rx;
#if LWIP_IPV6
  const IpCounter tx6 = lwip_stats.ip6.xmit, rx6 = lwip_stats.ip6.recv;
  dTx += static_cast<IpCounter>(tx6 - prevTx6);
  dRx += static_cast<IpCounter>(rx6 - prevRx6);
  prevTx6 = tx6;
  prevRx6 = rx6;
#endif
  if (dTx | dRx) PowerCounters::ipPackets(dTx, dRx);
#endif
}

// Arduino's event task.
void onWifiEvent(const arduino_event_id_t event) {
  if (event == ARDUINO_EVENT_WIFI_SCAN_DONE) PowerCounters::wifiScan();
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) PowerCounters::wifiConnect();
}

void writeRow(const char* name, const char* detail, const bool toRing) {
  PowerLogRow::Row r;
  Sample s;
  if (sampleFn) sampleFn(s);
  r.epoch = s.epoch;
  r.localOffsetS = s.localOffsetS;
  r.uptimeMs = millis();
  r.gen = PowerCounters::generation();
  r.event = name;
  r.detail = detail;
  r.pct = s.pct;
  r.pct256 = s.pct256;
  r.mv = s.mv;
  r.tempDeciC = s.tempDeciC;
  r.tempKnown = s.tempKnown;
  uint32_t panelAgeMs = 0;
  r.panelKnown = freeink::uc8179PanelTemperature(r.panelC, panelAgeMs) && panelAgeMs <= kPanelTempMaxAgeMs;
  r.chg = s.chg;
  r.usb = s.usb;
  r.chargerSeen = chargerSeen || s.chg || s.usb;
  chargerSeen = false;
  r.wifi = wifiState;
  r.t = PowerCounters::totals();
  const auto& counts = HalDisplay::refreshCounts().n;
  std::copy(std::begin(counts), std::end(counts), std::begin(r.refresh));
  const size_t n = PowerLogRow::format(rowBuf, sizeof(rowBuf), r);
  lastRowMs = millis();
  if (n == 0) {
    LOG_ERR("PWL", "row too long: %s", name);
    return;
  }
  if (toRing) append(rowBuf, static_cast<uint32_t>(n));
  LOG_INF("PWL", "%.*s", static_cast<int>(n - 1), rowBuf);
}
}  // namespace

void begin(const SampleFn sample, const bool deepSleepWake, const char* event, const char* detail) {
  if (begun) return;
  begun = true;
  sampleFn = sample;
  Sample s;
  if (sampleFn) sampleFn(s);
  PowerCounters::begin(s.epoch, deepSleepWake);
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_SCAN_DONE);
  WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_CONNECTED);
  wifiSinceMs = lastPollMs = millis();
  wifiState = sampleWifi();
  ensureRing();
  // After a restart or crash the ring may hold rows the card lacks.
  bootFlushPending = ringReady && ring.head != ring.aux;
  writeRow(event, detail, true);
}

void poll(const uint32_t idleMs) {
  if (!begun) return;
  const uint32_t nowMs = millis();
  if (nowMs - lastPollMs < kPollMs) return;
  lastPollMs = nowMs;
  const PowerCounters::RadioState before = wifiState;
  sampleRadio(nowMs);
  PowerCounters::tick();
  if ((before == PowerCounters::RADIO_OFF) != (wifiState == PowerCounters::RADIO_OFF)) {
    writeRow(wifiState == PowerCounters::RADIO_OFF ? "wifi_off" : "wifi_on", PowerLogRow::wifiName(wifiState), true);
  } else if (nowMs - lastRowMs >= static_cast<uint32_t>(KNOBS.powerTickMin) * 60000u) {
    writeRow("tick", nullptr, true);
  }
  if (!ringReady || idleMs < kFlushIdleMs) return;
  const uint32_t pending = ring.head - ring.aux;
  if (pending == 0) return;
  Sample s;
  if (sampleFn) sampleFn(s);
  const bool low = !s.usb && s.pct != 0 && s.pct <= kLowPct && pending >= kLowFlushBytes;
  if ((bootFlushPending || low || pending >= kRingBytes / 4 * 3) && flush()) bootFlushPending = false;
}

void event(const char* name, const char* detail) {
  if (!begun) return;
  sampleRadio(millis());  // the row's Wi-Fi time and state up to now
  writeRow(name, detail, true);
}

void noteCharger() { chargerSeen = true; }

void onSleep(const char* why, const bool chargeWake) {
  if (!begun) return;
  sampleRadio(millis());
  writeRow("sleep", why, true);
  Sample s;
  if (sampleFn) sampleFn(s);
  PowerCounters::noteSleep(s.epoch, chargeWake);
  PowerCounters::save(true);
  flush();
}

void onRestart() {
  if (!begun) return;
  // Runs on whichever task restarts (web server, OTA), so no row: rowBuf is
  // the main loop's, and the next boot's row carries these totals anyway. The
  // radio is not queried either, as the Wi-Fi shutdown handler may already
  // have stopped it: the time since the last sample goes to the state seen then.
  PowerCounters::wifiMs(wifiState, millis() - wifiSinceMs);
  wifiSinceMs = millis();
  PowerCounters::save(true);
}

void reset() {
  PowerCounters::reset();
  event("reset");
}

namespace {
// The live file becomes power.1.csv (one old copy).
bool rotate() {
  Storage.remove(OLD_LOG_PATH);
  if (!Storage.rename(LOG_PATH, OLD_LOG_PATH)) {
    LOG_ERR("PWL", "Failed to rotate %s", LOG_PATH);
    return false;
  }
  return true;
}

// False when the file starts with another firmware's columns: rows of this
// build must not land under them. Compared in DRAM chunks (SD reads are not
// handed PSRAM buffers). A missing or empty file matches.
bool headerMatches() {
  if (!Storage.exists(LOG_PATH)) return true;
  HalFile file = Storage.open(LOG_PATH, O_RDONLY);
  if (!file) return true;  // flush() reports the open failure
  constexpr size_t kLen = sizeof(PowerLogRow::kHeader) - 1;
  const bool empty = file.fileSize() == 0;
  bool same = empty || file.fileSize() >= kLen;
  char chunk[64];
  for (size_t at = 0; same && !empty && at < kLen;) {
    const size_t n = std::min(kLen - at, sizeof(chunk));
    same = file.read(chunk, n) == static_cast<int>(n) && memcmp(chunk, PowerLogRow::kHeader + at, n) == 0;
    at += n;
  }
  file.close();
  return same;
}
}  // namespace

bool flush() {
  if (!ensureRing() || !Storage.ready()) return false;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  uint32_t from = ring.aux;
  portEXIT_CRITICAL_SAFE(&ringMux);
  if (from == head) return true;
  if (head - from > kRingBytes) {
    LOG_ERR("PWL", "ring overran: %lu bytes lost", static_cast<unsigned long>(head - from - kRingBytes));
    from = head - kRingBytes;
  }
  Storage.ensureDirectoryExists("/debug/logs");
  if (!headerChecked) {
    if (!headerMatches()) {
      LOG_INF("PWL", "%s has other columns: moving it to %s", LOG_PATH, OLD_LOG_PATH);
      if (!rotate()) return false;  // checked again on the next flush
    }
    headerChecked = true;
  }
  HalFile file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("PWL", "Failed to open %s", LOG_PATH);
    return false;
  }
  if (file.fileSize() >= kMaxFileBytes) {
    file.close();
    if (!rotate()) return false;
    file = Storage.open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND);
    if (!file) {
      LOG_ERR("PWL", "Failed to open %s", LOG_PATH);
      return false;
    }
  }
  bool ok = true;
  if (file.fileSize() == 0) {
    ok = file.write(PowerLogRow::kHeader, sizeof(PowerLogRow::kHeader) - 1) == sizeof(PowerLogRow::kHeader) - 1;
  }
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
    LOG_ERR("PWL", "Failed to append to %s", LOG_PATH);
    return false;
  }
  portENTER_CRITICAL_SAFE(&ringMux);
  ring.aux = head;
  portEXIT_CRITICAL_SAFE(&ringMux);
  ring.writeBackHeader();
  return true;
}

}  // namespace PowerLog

#endif  // CROSSDINK_GOODIES && !SIMULATOR
