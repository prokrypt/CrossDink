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
constexpr uint32_t ARMED_MAGIC = 0x534C5241;  // "SLRA": the next sleep restarts

// RTC memory; power loss leaves garbage that fails the magic check.
RTC_NOINIT_ATTR uint32_t sleepRebootState;

bool logPath(char* out, const size_t size, const char* const prefix) {
  const char* sha = BuildInfo::gitSha();
  if (strcmp(sha, "unknown") == 0) return false;
  snprintf(out, size, "/debug/%s-%.8s.log", prefix, sha);
  return true;
}

// Copies the whole ring to the file; 512 B chunk on the stack, no heap.
void dumpRing(const char* path) {
  HalFile file = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
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
  dumpRing(path);
}

void armSleepReboot() { sleepRebootState = ARMED_MAGIC; }

void restartIfArmed() {
  if (sleepRebootState != ARMED_MAGIC) return;
  sleepRebootState = 0;
  LOG_INF("SLPLOG", "Sleep-reboot: restarting instead of powering down");
  ESP.restart();
}

}  // namespace SleepLog

#endif
