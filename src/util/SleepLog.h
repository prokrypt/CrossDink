#pragma once

// Debug builds: the first deep sleep of each build copies the whole PSRAM log
// to /debug/sleep-<sha8>.log (deep sleep wipes the ring; the wake's own lines
// are already in the normal log). An existing file means this build already
// logged a sleep, so nothing is written.
//
// Goodies > Sleep-reboot-log (after a confirm) runs the same sleep path but
// restarts at the end instead of powering down. A restart keeps the ring, so
// the log up to the restart (no boot lines) is written on the first main-loop
// pass after it, to /debug/sleep-reboot-<sha8>.log (replaced each run); the
// sleep itself writes nothing extra.
#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)
namespace SleepLog {
// Just before Storage.shutdown() in enterDeepSleep(): one SD stat, plus one
// write of the whole ring (up to 512 KB) the first time per build; nothing
// when a sleep-reboot is armed.
void onSleep();
// Goodies: the next enterDeepSleep() restarts instead of sleeping.
void armSleepReboot();
// Last step of enterDeepSleep(), in place of the power-down: restarts when a
// sleep-reboot is armed (does not return), else returns.
void restartIfArmed();
// Every main-loop pass: writes the ring after a sleep-reboot, once.
void loop();
}  // namespace SleepLog
#else
namespace SleepLog {
inline void onSleep() {}
inline void armSleepReboot() {}
inline void restartIfArmed() {}
inline void loop() {}
}  // namespace SleepLog
#endif
