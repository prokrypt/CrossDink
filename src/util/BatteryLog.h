#pragma once

#include <cstdint>

// Goodies builds (X4 Pro): event-driven battery log plus the counters behind
// Goodies > Battery & stats.
//
// Rows go to a 64 KB PSRAM ring that survives restarts and crashes (not deep
// sleep or power loss) and are appended to /debug/logs/battery.csv, as
//   epoch_utc,local_time,uptime_ms,pct,mv,chg,usb,temp_c,light_pct,event,detail
// Flushed before deep sleep, and from the main loop after 2 s without input
// once a boot left rows, the ring is 3/4 full, or the battery is at 5% or
// less off USB. At 256 KB the file becomes /debug/logs/battery.1.csv (one old copy).
// The row's battery fields are the last main-loop reading, so any task may log.
namespace BatteryLog {

// Kept in RTC memory: survives deep sleep and restarts, cleared on power loss
// or by reset(). The "since last charged" fields restart when charging stops
// (charge done, or the cable pulled while charging), awake or asleep.
struct Stats {
  uint32_t magic;
  uint16_t version;
  uint16_t size;   // sizeof(Stats)
  uint32_t boots;  // power-on, crash and restart boots
  uint32_t wakes;  // deep-sleep wakes
  uint32_t awakeS;
  uint32_t asleepS;
  // Since last charged.
  uint32_t chargedEpoch;  // 0 = not seen since reset
  uint32_t battAwakeS;
  uint32_t battAsleepS;
  uint32_t dropAwakePct;
  uint32_t dropAsleepPct;
  uint16_t chargedPct;
  // Set at sleep and at each charge wake, used at the next wake.
  uint16_t sleepPct;
  uint32_t sleepEpoch;
  bool sleepUsb;
  // Power-button wakes too short to boot (Short Power Button not set to wake):
  // all time, and since the last real wake with the ms they spent awake.
  uint32_t falseWakes;
  uint32_t pendingFalseWakes;
  uint32_t pendingFalseWakeMs;
  // Charge starts/stops seen asleep (charger STAT wakes), logged at the next real wake.
  struct SleepEvent {
    uint32_t epoch;
    uint16_t mv;
    uint8_t pct;
    bool chg;
  };
  uint32_t sleepEventCount;
  SleepEvent sleepEvents[16];
  uint32_t crc;  // over everything above; RTC bytes that fail it are discarded
};

#if CROSSDINK_GOODIES && !defined(SIMULATOR)
// After SD and SETTINGS are up: the boot row and the wake/boot counters.
void onBoot();
// Last thing before Storage.shutdown(): the sleep row, then a flush.
void onSleep(const char* why);
// Early in setup, on a charger STAT wake: note the charge start/stop in RTC
// memory, then the caller sleeps again (no display, SD or settings).
void onChargeWake();
// Early in setup, on a power-button wake that goes straight back to sleep.
void noteFalseWake();
// Main loop, every pass (runs once a second): %, charger, USB, Wi-Fi and
// frontlight rows, awake time, and the idle flush.
void poll(uint32_t idleMs);
// Any task.
void event(const char* name, const char* detail = nullptr);
// Main loop. A user light change (on release, not per slider step) or the Light
// Timeout (timedOut: dark; its restore passes false). Ducks and transfer pulses
// never call it. Logs a "light" row when the level differs from the last one.
void lightChanged(bool timedOut = false);
// Main loop only. Appends the unflushed rows to the SD file.
bool flush();
// Any task: hands the rows not yet on the SD card to sink, oldest first, in
// chunks (they follow the file's last row). Read-only; the ring keeps them.
void forEachPending(void (*sink)(void* ctx, const char* data, uint32_t len), void* ctx);
const Stats& stats();
// UTC seconds from the RTC, 0 when it is not set.
uint32_t nowEpoch();
// Clears Stats and the display refresh counts (not the SD file).
void reset();
#else
inline void onBoot() {}
inline void onSleep(const char*) {}
inline void onChargeWake() {}
inline void noteFalseWake() {}
inline void poll(uint32_t) {}
inline void event(const char*, const char* = nullptr) {}
inline void lightChanged(bool = false) {}
#endif

constexpr char LOG_PATH[] = "/debug/logs/battery.csv";
constexpr char OLD_PATH[] = "/debug/logs/battery.1.csv";  // LOG_PATH rotates here at 256 KB

}  // namespace BatteryLog
