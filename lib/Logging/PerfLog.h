#pragma once

#include <cstdint>

// Debug-only timing and I/O counters behind compact, greppable log lines
// (recipes in the device LOG-GUIDE):
//   [LAT]  input -> render -> ink per user action
//   [PERF] SD opens/bytes/ms and image cache hits/misses/decode ms per 2 s
//   [BOOT] first ink after reset, and silent restart request -> first ink
//   [PM]   light-sleep residency and PM lock hold times (CONFIG_PM_PROFILING)
//
// Enabled with -DCROSSDINK_PERF_LOG=1 (x4-pro-light-sleep-debug). Elsewhere
// every function is an empty inline, so call sites cost nothing.
#if CROSSDINK_PERF_LOG && !defined(SIMULATOR)
namespace PerfLog {
// A press, tap or tilt starts a latency sample. A release restarts it only
// while no render has begun, so release-triggered actions time from release.
// kind ("btn", "tap", "swipe", ...) is a string literal shown as [LAT] in=.
void noteInput(bool release, const char* kind);
// Render task brackets around Activity::render().
void noteRenderStart();
void noteRenderEnd(const char* activity);
// Reader: how the page frame was produced ("pre" drawn ahead, "draw" composed).
void notePagePath(const char* path);
// A refresh finished on the panel: closes the pending latency sample, and the
// first one after reset logs [BOOT].
void noteInk();
// A silent restart is about to happen; the first ink after it logs the time
// from this call across the reset (RTC timer, survives software restart).
void noteRestart();
// SD activity (HalStorage) and image decode/cache results (ImageBlock).
void noteSdOpen(bool opened);
void noteSdRead(uint32_t bytes, uint32_t us);
void noteSdWrite(uint32_t bytes, uint32_t us);
void noteImage(bool cacheHit, uint32_t ms);
// One [PERF] line if anything changed since the last call; [PM] every 30 s.
void logPeriodic();
}  // namespace PerfLog
#else
namespace PerfLog {
inline void noteInput(bool, const char*) {}
inline void noteRenderStart() {}
inline void noteRenderEnd(const char*) {}
inline void notePagePath(const char*) {}
inline void noteInk() {}
inline void noteRestart() {}
inline void noteSdOpen(bool) {}
inline void noteSdRead(uint32_t, uint32_t) {}
inline void noteSdWrite(uint32_t, uint32_t) {}
inline void noteImage(bool, uint32_t) {}
inline void logPeriodic() {}
}  // namespace PerfLog
#endif
