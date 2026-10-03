#pragma once

#include <cstddef>

// Per-core CPU load and the busiest tasks since the previous call, as the last
// part of the 2 s [SYS] line, plus a [STK] line with the tasks with the least
// stack headroom about every 30 s.
// Opt-in per env with -DCROSSDINK_CORE_LOAD_LOG=1 (x4-pro-debug);
// needs FreeRTOS run-time stats and trace facility, which the S3 SDK enables.
// Elsewhere it is an empty inline.
#if CROSSDINK_CORE_LOAD_LOG && !defined(SIMULATOR)
namespace CoreLoadLog {
// Writes " core0 X% core1 Y% over N ms act=A | tasks" to out (up to 5 tasks,
// each >= 10% of a core), or "" when both cores were under 5% or there is no
// previous sample.
void formatSinceLast(char* out, size_t size);
// [PM] window hook (PerfLog::setPmWindowHook): when a window had no input but
// a core was out of idle >= 5%, logs the busiest tasks over that window.
void logQuietWindowTasks(unsigned rtos0Pct, unsigned rtos1Pct, long gpioWakes);
}  // namespace CoreLoadLog
#else
namespace CoreLoadLog {
inline void formatSinceLast(char* out, size_t size) {
  if (size > 0) out[0] = '\0';
}
inline void logQuietWindowTasks(unsigned, unsigned, long) {}
}  // namespace CoreLoadLog
#endif
