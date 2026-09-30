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
// or by reset(). The "since unplug" fields restart when USB is unplugged.
struct Stats {
  uint32_t magic;
  uint32_t boots;  // power-on, crash and restart boots
  uint32_t wakes;  // deep-sleep wakes
  uint32_t awakeS;
  uint32_t asleepS;
  // Since unplug.
  uint32_t unplugEpoch;  // 0 = not unplugged since reset
  uint32_t battAwakeS;
  uint32_t battAsleepS;
  uint32_t dropAwakePct;
  uint32_t dropAsleepPct;
  uint16_t unplugPct;
  // Set at sleep, used at the next wake.
  uint16_t sleepPct;
  uint32_t sleepEpoch;
  bool sleepUsb;
};

#if CROSSDINK_GOODIES && !defined(SIMULATOR)
// After SD and SETTINGS are up: the boot row and the wake/boot counters.
void onBoot();
// Last thing before Storage.shutdown(): the sleep row, then a flush.
void onSleep(const char* why);
// Main loop, every pass (runs once a second): %, charger, USB, Wi-Fi and
// frontlight rows, awake time, and the idle flush.
void poll(uint32_t idleMs);
// Any task.
void event(const char* name, const char* detail = nullptr);
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
inline void poll(uint32_t) {}
inline void event(const char*, const char* = nullptr) {}
#endif

constexpr char LOG_PATH[] = "/debug/logs/battery.csv";

}  // namespace BatteryLog
