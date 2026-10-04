#pragma once

// Debug builds: the first deep sleep of each build copies the PSRAM log tail
// (the sleep-entry lines) to /debug/sleep-<sha8>.log, and the wake after it
// appends the boot lines. The ring is wiped by deep sleep, so each half is
// written while its lines still exist. An existing file means this build
// already logged a sleep, so nothing is written.
#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)
namespace SleepLog {
// Just before Storage.shutdown() in enterDeepSleep(): one SD stat, plus one
// ~16 KB write the first time per build.
void onSleep();
// Every main-loop pass: appends the wake half once the boot has settled.
void loop();
}  // namespace SleepLog
#else
namespace SleepLog {
inline void onSleep() {}
inline void loop() {}
}  // namespace SleepLog
#endif
