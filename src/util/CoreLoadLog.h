#pragma once

// Per-core CPU load and the busiest tasks since the previous call, as one log
// line. Only compiled in when the SDK is built with FreeRTOS run-time stats
// (the x4-pro-light-sleep-debug env); elsewhere it is an empty inline.
#if !defined(SIMULATOR)
#include <sdkconfig.h>
#endif

#if !defined(SIMULATOR) && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_USE_TRACE_FACILITY
#define CROSSINK_CORE_LOAD_LOG 1
namespace CoreLoadLog {
void logSinceLast();
}
#else
namespace CoreLoadLog {
inline void logSinceLast() {}
}  // namespace CoreLoadLog
#endif
