#pragma once

#include <cstdint>

// Debug-only timing and I/O counters behind compact, greppable log lines
// (recipes in the device LOG-GUIDE):
//   [LAT]  input -> render -> ink per user action
//   [PERF] SD opens/bytes/ms and image cache hits/misses/decode ms per 2 s
//   [BOOT] first ink after reset, and silent restart request -> first ink
//   [PM]   light-sleep residency, wake counts and PM lock hold times per
//          activity (CONFIG_PM_PROFILING); a window ends every 30 s and on
//          every activity change
//
// Enabled with -DCROSSDINK_PERF_LOG=1 (x4-pro-debug). Elsewhere
// every function is an empty inline, so call sites cost nothing.
#if CROSSDINK_PERF_LOG && !defined(SIMULATOR)
namespace PerfLog {
// A press, tap or tilt starts a latency sample. A release restarts it only
// while no render has begun, so release-triggered actions time from release.
// kind ("btn", "tap", "swipe", ...) is a string literal shown as [LAT] in=.
void noteInput(bool release, const char* kind);
// Render task brackets around Activity::render(). Start takes the activity
// name while the render lock is held; after render() the lock is gone and the
// activity may already be destroyed.
void noteRenderStart(const char* activity);
void noteRenderEnd();
// Reader: how the page frame was produced ("pre" drawn ahead, "draw" composed).
void notePagePath(const char* path);
// A refresh finished on the panel: closes the pending latency sample, and the
// first one after reset logs [BOOT].
void noteInk();
// Boot phase mark (name is a string literal): the first ink logs
// "[BOOT] t <name>=<ms since previous mark> ... ink= first_ink=".
void noteBootPhase(const char* name);
// A silent restart is about to happen; the first ink after it logs the time
// from this call across the reset (RTC timer, survives software restart).
void noteRestart();
// SD activity (HalStorage) and image decode/cache results (ImageBlock).
void noteSdOpen(bool opened);
void noteSdRead(uint32_t bytes, uint32_t us);
void noteSdWrite(uint32_t bytes, uint32_t us);
void noteImage(bool cacheHit, uint32_t ms);
// One [PERF] line if anything changed since the last call; [PM] every 30 s
// and when the rendered activity changed.
void logPeriodic();
// Name of the activity rendered last (render task), for per-activity lines.
void currentActivity(char* out, uint32_t size);
// Source of input wake counts for [PM]: returns and clears the button and
// touch line interrupts since the previous call.
using WakeCountFn = void (*)(uint32_t& buttons, uint32_t& touch);
void setWakeCounter(WakeCountFn fn);
}  // namespace PerfLog
#else
namespace PerfLog {
inline void noteInput(bool, const char*) {}
inline void noteRenderStart(const char*) {}
inline void noteRenderEnd() {}
inline void notePagePath(const char*) {}
inline void noteInk() {}
inline void noteBootPhase(const char*) {}
inline void noteRestart() {}
inline void noteSdOpen(bool) {}
inline void noteSdRead(uint32_t, uint32_t) {}
inline void noteSdWrite(uint32_t, uint32_t) {}
inline void noteImage(bool, uint32_t) {}
inline void logPeriodic() {}
inline void currentActivity(char* out, uint32_t size) {
  if (size > 0) out[0] = '\0';
}
using WakeCountFn = void (*)(uint32_t& buttons, uint32_t& touch);
inline void setWakeCounter(WakeCountFn) {}
}  // namespace PerfLog
#endif
