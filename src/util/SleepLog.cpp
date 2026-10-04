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
constexpr uint32_t WAKE_PENDING_MAGIC = 0x534C4C47;  // "SLLG"
constexpr uint32_t SLEEP_TAIL_BYTES = 16384;
constexpr uint32_t WAKE_SETTLE_MS = 15000;  // boot, first render and Wi-Fi/OPDS starts are in the ring by then

// Set only when this sleep created the file; RTC memory survives deep sleep,
// power loss leaves garbage that fails the magic check.
RTC_NOINIT_ATTR uint32_t wakeHalfPending;

bool logPath(char* out, const size_t size) {
  const char* sha = BuildInfo::gitSha();
  if (strcmp(sha, "unknown") == 0) return false;
  snprintf(out, size, "/debug/sleep-%.8s.log", sha);
  return true;
}

// Copies ring text from `from` to the file; 512 B chunk on the stack, no heap.
bool appendRing(const char* path, const char* header, const uint32_t from) {
  HalFile file = Storage.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOG_ERR("SLPLOG", "open %s failed", path);
    return false;
  }
  file.write(header, strlen(header));
  const uint32_t end = PsramLog::end();
  uint32_t cursor = std::max(PsramLog::oldest(), from);
  char chunk[512];
  size_t n;
  bool ok = true;
  while (cursor < end && (n = PsramLog::read(cursor, chunk, std::min<size_t>(sizeof(chunk), end - cursor))) > 0) {
    if (file.write(chunk, n) != n) {
      LOG_ERR("SLPLOG", "write %s failed", path);
      ok = false;
      break;
    }
  }
  file.close();
  return ok;
}
}  // namespace

namespace SleepLog {

void onSleep() {
  char path[40];
  if (!logPath(path, sizeof(path)) || Storage.exists(path)) return;
  Storage.ensureDirectoryExists("/debug");
  const uint32_t end = PsramLog::end();
  char header[64];
  snprintf(header, sizeof(header), "=== sleep half, %s ===\n", BuildInfo::gitSha());
  if (appendRing(path, header, end > SLEEP_TAIL_BYTES ? end - SLEEP_TAIL_BYTES : 0)) wakeHalfPending = WAKE_PENDING_MAGIC;
}

void loop() {
  if (wakeHalfPending != WAKE_PENDING_MAGIC || millis() < WAKE_SETTLE_MS) return;
  wakeHalfPending = 0;  // once, even if the write fails
  char path[40];
  if (!logPath(path, sizeof(path))) return;
  appendRing(path, "\n=== wake half (boot to +15 s) ===\n", 0);
}

}  // namespace SleepLog

#endif
