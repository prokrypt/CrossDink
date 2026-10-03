#pragma once

#include <cstdint>

namespace PerfLog {
struct LightSleepStats {
  uint32_t sleeps, rejects, upS;
  uint8_t sleepPct;             // share of upS spent in light sleep
  uint32_t rejectCause;         // rjc= bits of the last rejected sleep, 0 = none
  const char* rejectCauseName;  // its lowest bit ("GPIO", "timer", ...), "-" for none
};
}  // namespace PerfLog

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
// Id for one main-loop frame with input: its [IN] lines and the [LAT] line for
// the sample it starts all carry #N.
uint32_t nextInputSeq();
// A press, tap or tilt starts a latency sample. A release restarts it only
// while no render has begun, so release-triggered actions time from release.
// Contact moves ("touch") never replace a pending sample. kind ("btn", "tap",
// "swipe", ...) is a string literal shown as [LAT] in=.
void noteInput(bool release, const char* kind, uint32_t seq);
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
// Deep sleep is about to start: reason and activity go to RTC memory (survives
// deep sleep, not power loss) and the next boot prints them with logLastSleep()
// as "[BOOT] last sleep: ...", since the PSRAM log ring does not survive.
void noteDeepSleep(const char* reason, const char* activity);
// Also prints the RTC event trail ("[BOOT] trail: ...") left by the previous
// boots: the last 8 activity entries and the last [ERR] line, which survive
// deep sleep and restarts (not power loss) while the PSRAM ring does not.
void logLastSleep();
// Trail entries (RTC slow memory, CRC-guarded; name is copied).
void noteActivity(const char* name);
void noteError(const char* line);
// "[ERS] open: <stage>=<ms> ... other= total=" once per book open, from
// bookOpenBegin() (the reader starts loading) to bookOpenEnd() (first page on
// the panel, or the open failed). Stages add up under a string-literal name
// and are dropped outside an open.
void bookOpenBegin();
void bookOpenStage(const char* name, uint32_t ms);
void bookOpenEnd();
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
// A worker task is about to delete itself: logs "[STK] exit <name>: min free
// N" (bytes of stack never used) when N is a new low for that name this boot.
// name is a string literal.
void noteTaskExit(const char* name);
// One main-loop pass; [PM] prints the count per window as loop=N, so timer
// wakes split into loop ticks and everything else.
void noteLoopPass();
// Called after each [PM] line with the per-core rtos lock share of the window
// and its GPIO wakes, so the app can name the tasks behind a busy idle window.
using PmWindowFn = void (*)(unsigned rtos0Pct, unsigned rtos1Pct, long gpioWakes);
void setPmWindowHook(PmWindowFn fn);
// Writes the armed wake lines ("pin armed/now" each) for the line [PM] logs
// when a window's sleeps were all rejected.
using WakePinsFn = void (*)(char* out, uint32_t size);
void setWakePinsDescriber(WakePinsFn fn);
// Light sleep since boot, as of the last [PM] window (every 30 s and on each
// activity change). False without CONFIG_PM_PROFILING or before the first window.
bool lightSleepStats(LightSleepStats& out);
}  // namespace PerfLog
#else
namespace PerfLog {
inline bool lightSleepStats(LightSleepStats&) { return false; }
inline uint32_t nextInputSeq() { return 0; }
inline void noteInput(bool, const char*, uint32_t) {}
inline void noteRenderStart(const char*) {}
inline void noteRenderEnd() {}
inline void notePagePath(const char*) {}
inline void noteInk() {}
inline void noteBootPhase(const char*) {}
inline void noteRestart() {}
inline void noteDeepSleep(const char*, const char*) {}
inline void logLastSleep() {}
inline void noteActivity(const char*) {}
inline void noteError(const char*) {}
inline void bookOpenBegin() {}
inline void bookOpenStage(const char*, uint32_t) {}
inline void bookOpenEnd() {}
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
inline void noteTaskExit(const char*) {}
inline void noteLoopPass() {}
using PmWindowFn = void (*)(unsigned rtos0Pct, unsigned rtos1Pct, long gpioWakes);
inline void setPmWindowHook(PmWindowFn) {}
using WakePinsFn = void (*)(char* out, uint32_t size);
inline void setWakePinsDescriber(WakePinsFn) {}
}  // namespace PerfLog
#endif
