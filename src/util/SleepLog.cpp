#include "SleepLog.h"

#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PsramLog.h>
#include <esp_attr.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BuildInfo.h"

namespace {
constexpr uint32_t ARMED_MAGIC = 0x534C5241;         // "SLRA": the next sleep restarts
constexpr uint32_t DUMP_PENDING_MAGIC = 0x534C5244;  // "SLRD": restarted, ring not yet written
constexpr uint32_t DUMP_SETTLE_MS = 15000;           // boot, first render and Wi-Fi/OPDS starts are in the ring by then

// RTC memory survives the restart; power loss leaves garbage that fails the magic checks.
RTC_NOINIT_ATTR uint32_t sleepRebootState;

bool logPath(char* out, const size_t size, const char* const prefix) {
  const char* sha = BuildInfo::gitSha();
  if (strcmp(sha, "unknown") == 0) return false;
  snprintf(out, size, "/debug/%s-%.8s.log", prefix, sha);
  return true;
}

// Copies the whole ring to the file; 512 B chunk on the stack, no heap.
void dumpRing(const char* path, const bool replace) {
  HalFile file = Storage.open(path, O_WRONLY | O_CREAT | (replace ? O_TRUNC : O_APPEND));
  if (!file) {
    LOG_ERR("SLPLOG", "open %s failed", path);
    return;
  }
  const uint32_t end = PsramLog::end();
  uint32_t cursor = PsramLog::oldest();
  char chunk[512];
  size_t n;
  while (cursor < end && (n = PsramLog::read(cursor, chunk, std::min<size_t>(sizeof(chunk), end - cursor))) > 0) {
    if (file.write(chunk, n) != n) {
      LOG_ERR("SLPLOG", "write %s failed", path);
      break;
    }
  }
  file.close();
}
}  // namespace

namespace SleepLog {

void onSleep() {
  char path[48];
  if (sleepRebootState == ARMED_MAGIC || !logPath(path, sizeof(path), "sleep") || Storage.exists(path)) return;
  Storage.ensureDirectoryExists("/debug");
  dumpRing(path, false);
}

void armSleepReboot() { sleepRebootState = ARMED_MAGIC; }

void restartIfArmed() {
  if (sleepRebootState != ARMED_MAGIC) return;
  sleepRebootState = DUMP_PENDING_MAGIC;
  LOG_INF("SLPLOG", "Sleep-reboot: restarting instead of powering down");
  ESP.restart();
}

void loop() {
  if (sleepRebootState != DUMP_PENDING_MAGIC || millis() < DUMP_SETTLE_MS) return;
  sleepRebootState = 0;  // once, even if the write fails
  char path[48];
  if (!logPath(path, sizeof(path), "sleep-reboot")) return;
  Storage.ensureDirectoryExists("/debug");
  dumpRing(path, true);
  LOG_INF("SLPLOG", "Sleep-reboot: PSRAM log saved to %s", path);
}

}  // namespace SleepLog

#endif
