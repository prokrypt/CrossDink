#pragma once

// Per-core CPU load and the busiest tasks since the previous call, as one log
// line, plus the tasks with the least stack headroom about every 30 s.
// Opt-in per env with -DCROSSDINK_CORE_LOAD_LOG=1 (x4-pro-debug);
// needs FreeRTOS run-time stats and trace facility, which the S3 SDK enables.
// Elsewhere it is an empty inline.
#if CROSSDINK_CORE_LOAD_LOG && !defined(SIMULATOR)
namespace CoreLoadLog {
void logSinceLast();
}  // namespace CoreLoadLog
#else
namespace CoreLoadLog {
inline void logSinceLast() {}
}  // namespace CoreLoadLog
#endif
