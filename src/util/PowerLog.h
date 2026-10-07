#pragma once

#include <cstdint>

// Goodies builds (X4 Pro): /debug/logs/power.csv, one row of cumulative
// PowerCounters per event, for scripts/power_fit.py to work out the drain of
// each function from the gauge's % alone.
//
// Rows are written at wake/boot and sleep, when the whole % changes, when
// Wi-Fi turns on or off, every Goodies > Knobs powerTickMin while awake, and
// for Power test marks. The counters are cumulative, so any two rows of the
// same generation (gen) give the activity between them; a new generation
// (power loss, crash, stats reset) starts over at zero.
//
// Rows go to a 16 KB PSRAM ring (survives restarts, not deep sleep or power
// loss) and are appended to the file before deep sleep and from the main loop
// after 2 s without input once a boot left rows or the ring is 3/4 full. At
// 256 KB the file becomes power.1.csv (one old copy).
//
// BatteryLog drives it: it owns the gauge readings each row carries.
namespace PowerLog {

// The battery side of a row, from BatteryLog's last main-loop reading.
struct Sample {
  uint32_t epoch = 0;  // UTC seconds, 0 = clock not set
  int32_t localOffsetS = 0;
  uint16_t pct = 0;
  uint16_t pct256 = 0;  // 1/256 % when the gauge gives a fraction, else 0
  uint16_t mv = 0;
  int16_t tempDeciC = 0;
  bool tempKnown = false;
  bool chg = false;
  bool usb = false;
};
using SampleFn = void (*)(Sample& out);

constexpr char LOG_PATH[] = "/debug/logs/power.csv";
constexpr char OLD_LOG_PATH[] = "/debug/logs/power.1.csv";

#if CROSSDINK_GOODIES && !defined(SIMULATOR)
// Main loop, from BatteryLog::onBoot(): restores the counters and writes the
// wake or boot row. deepSleepWake: this boot ended a deep sleep.
void begin(SampleFn sample, bool deepSleepWake, const char* event, const char* detail);
// Main loop, once a second: Wi-Fi time, IP packets, per-core busy time, the
// periodic row and the idle flush.
void poll(uint32_t idleMs);
// Main loop: one row now. name is the event column ("pct", "test_start", ...).
void event(const char* name, const char* detail = nullptr);
// Charging or USB power seen: the next row says so, and the fit skips the
// interval that ends there.
void noteCharger();
// Sleep path, after the battery log's sleep row: row, counters to RTC memory,
// flush. chargeWake: the charger STAT line is armed to wake the device.
void onSleep(const char* why, bool chargeWake);
// Restart shutdown handler: counters to RTC memory and a row to the ring.
void onRestart();
// Battery & stats reset: counters to zero in a new generation.
void reset();
// Main loop: unflushed rows to the SD file.
bool flush();
#else
inline void begin(SampleFn, bool, const char*, const char*) {}
inline void poll(uint32_t) {}
inline void event(const char*, const char* = nullptr) {}
inline void noteCharger() {}
inline void onSleep(const char*, bool) {}
inline void onRestart() {}
inline void reset() {}
inline bool flush() { return true; }
#endif

}  // namespace PowerLog
