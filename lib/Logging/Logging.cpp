#include "Logging.h"

#include <BoardConfig.h>
#include <PerfLog.h>
#include <PsramLog.h>
#include <esp_rom_sys.h>
#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)
#include <esp_log.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#ifdef SIMULATOR
#include <Arduino.h>

MySerialImpl MySerialImpl::instance;

size_t MySerialImpl::write(uint8_t b) { return logSerial.write(b); }
size_t MySerialImpl::write(const uint8_t* buffer, size_t size) { return logSerial.write(buffer, size); }
void MySerialImpl::flush() { logSerial.flush(); }
#endif

#define MAX_ENTRY_LEN 256
// A log line waits at most this long for a reply or stream holding the port.
static constexpr uint32_t LOG_LINE_LOCK_WAIT_MS = 2;
#if CROSSDINK_PSRAM_LOG
// The crash report reads the PSRAM ring (HalSystem::checkPanic), so the RTC
// copy only needs a few short lines; this frees ~3.5 KB of RTC slow memory.
#define MAX_LOG_LINES 4
#define RTC_ENTRY_LEN 128
#else
#define MAX_LOG_LINES 16
#define RTC_ENTRY_LEN MAX_ENTRY_LEN
#endif

// Simple ring buffer log, useful for error reporting when we encounter a crash
RTC_NOINIT_ATTR char logMessages[MAX_LOG_LINES][RTC_ENTRY_LEN];
RTC_NOINIT_ATTR size_t logHead = 0;
// Magic word written alongside logHead to detect uninitialized RTC memory.
// RTC_NOINIT_ATTR is not zeroed on cold boot, so logHead may appear in-range
// (0..MAX_LOG_LINES-1) by chance even though logMessages is garbage. The magic
// value is only set by clearLastLogs(), so its absence means the buffer was
// never properly initialized.
RTC_NOINIT_ATTR uint32_t rtcLogMagic;
static constexpr uint32_t LOG_RTC_MAGIC = 0xDEADBEEF;

// logPrintf runs on both cores at once (loop on core 0, render on core 1), so
// the ring's head and slots need a lock. A spinlock, because logging happens
// before the scheduler starts and must never block.
#ifdef SIMULATOR
#define LOG_RING_LOCK()
#define LOG_RING_UNLOCK()
#else
static portMUX_TYPE logRingMux = portMUX_INITIALIZER_UNLOCKED;
#define LOG_RING_LOCK() portENTER_CRITICAL_SAFE(&logRingMux)
#define LOG_RING_UNLOCK() portEXIT_CRITICAL_SAFE(&logRingMux)
#endif

void addToLogRingBuffer(const char* message) {
  // Add the message to the ring buffer, overwriting old messages if necessary.
  // If the magic is wrong or logHead is out of range (RTC_NOINIT_ATTR garbage
  // on cold boot), clear the entire buffer so subsequent reads are safe.
  LOG_RING_LOCK();
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    memset(logMessages, 0, sizeof(logMessages));
    logHead = 0;
    rtcLogMagic = LOG_RTC_MAGIC;
  }
  char* entry = logMessages[logHead];
  strncpy(entry, message, RTC_ENTRY_LEN - 1);
  entry[RTC_ENTRY_LEN - 1] = '\0';
  // Cut off: keep a newline so getLastLogs() doesn't join it to the next line.
  if (entry[RTC_ENTRY_LEN - 2] != '\0') entry[RTC_ENTRY_LEN - 2] = '\n';
  logHead = (logHead + 1) % MAX_LOG_LINES;
  LOG_RING_UNLOCK();
}

// Writes one finished line to the console, or skips it when the port is busy
// or the host isn't reading. The RTC and PSRAM rings are the caller's job.
static void logSerialEmit(const char* buf, const size_t len) {
#if defined(SIMULATOR)
  (void)len;
  std::fputs(buf, stderr);
#elif FREEINK_LOG_TRANSPORT == FREEINK_LOG_TRANSPORT_ROM_PRINTF
  // IDF/ROM console path for boards monitored over USB-Serial-JTAG, where the
  // HWCDC `operator bool` reads false under `pio device monitor` and logs would
  // otherwise be silently dropped (e.g. Sticky).
  (void)len;
  esp_rom_printf("%s", buf);
#else
  if (logSerialHostConnected() && logSerialLock(LOG_LINE_LOCK_WAIT_MS)) {
#if LOG_SERIAL_HAS_TX_TIMEOUT
    // A host that is attached but not reading leaves the TX ring full: skip the
    // line rather than wait out the TX timeout on every log call. The RTC and
    // PSRAM rings still get it.
    if (static_cast<size_t>(logSerial.availableForWrite()) >= len)
#endif
      logSerial.print(buf);
    logSerialUnlock();
  }
#endif
}

// Since logging can take a large amount of flash, we want to make the format string as short as possible.
// This logPrintf prepend the timestamp, level and origin to the user-provided message, so that the user only needs to
// provide the format string for the message itself.
void logPrintf(const char* level, const char* origin, const char* format, ...) {
  va_list args;
  va_start(args, format);
  char buf[MAX_ENTRY_LEN];
  char* c = buf;
  // add timestamp, level and origin
  {
    unsigned long ms = millis();
    int len = snprintf(c, sizeof(buf), "[%lu] [%s] [%s] ", ms, level, origin);
    // error while writing => return
    if (len < 0) {
      va_end(args);
      return;
    }
    // clamp c to be in buffer range
    c += std::min(len, MAX_ENTRY_LEN);
  }
  // add the user message
  {
    const size_t room = sizeof(buf) - (c - buf);
    int len = vsnprintf(c, room, format, args);
    if (len < 0) {
      va_end(args);
      return;
    }
    // Cut off: keep the format's trailing newline so the next entry starts on its own line.
    if (static_cast<size_t>(len) >= room) buf[sizeof(buf) - 2] = '\n';
  }
  va_end(args);
  const size_t len = strnlen(buf, sizeof(buf));
  logSerialEmit(buf, len);
  addToLogRingBuffer(buf);
  PsramLog::append(buf, len);
  if (strcmp(level, "ERR") == 0) PerfLog::noteError(buf);  // [WRN] is not an error
}

#if defined(SIMULATOR)
void logSerialInit() {}
bool logSerialHostConnected() { return static_cast<bool>(logSerial); }
bool logSerialLock(uint32_t) { return true; }
void logSerialUnlock() {}

bool logSerialWriteAll(const uint8_t* data, const size_t len, uint32_t, uint32_t) {
  return logSerial.write(data, len) == len;  // the simulator transport never drops
}
#else
namespace {
constexpr uint32_t NO_HOST_STALL_MS = 10;

// Before the scheduler runs, or in an ISR, there is no task to own a mutex;
// writes there go straight out, as before.
bool canUseLogSerialMutex() { return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && !xPortInIsrContext(); }

StaticSemaphore_t logSerialMutexStorage;
SemaphoreHandle_t logSerialMutex = nullptr;  // null until logSerialInit()
}  // namespace

bool logSerialHostConnected() {
  // Connects at once so boot logs aren't held back. ponytail: unlocked
  // statics; a race between tasks can only delay a disconnect.
  static bool connected = false;
  static uint32_t lostAtMs = 0;  // when the raw flag went false, 0 = it is true
  if (logSerial) {
    connected = true;
    lostAtMs = 0;
  } else if (lostAtMs == 0) {
    lostAtMs = millis() | 1;
  } else if (millis() - lostAtMs >= 1000) {
    connected = false;
  }
  return connected;
}

#if CROSSDINK_PSRAM_LOG
namespace {
// ESP-IDF's own lines (E (1234) wifi: ...) go to the same console and PSRAM
// ring as logPrintf, so a remote dump shows them next to the app's lines.
// IDF never calls esp_log from an ISR (early logs use the ROM printf).
int espLogVprintf(const char* format, va_list args) {
  char buf[MAX_ENTRY_LEN];
  const int n = vsnprintf(buf, sizeof(buf), format, args);
  if (n <= 0) return n;
  const size_t len = std::min<size_t>(n, sizeof(buf) - 1);
  logSerialEmit(buf, len);
  PsramLog::append(buf, len);
  return n;
}
}  // namespace
#endif

void logSerialInit() {
  if (logSerialMutex == nullptr) logSerialMutex = xSemaphoreCreateRecursiveMutexStatic(&logSerialMutexStorage);
#if CROSSDINK_PSRAM_LOG
  esp_log_set_vprintf(espLogVprintf);
#endif
}

bool logSerialLock(const uint32_t waitMs) {
  if (logSerialMutex == nullptr || !canUseLogSerialMutex()) return true;
  return xSemaphoreTakeRecursive(logSerialMutex, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void logSerialUnlock() {
  if (logSerialMutex != nullptr && canUseLogSerialMutex()) xSemaphoreGiveRecursive(logSerialMutex);
}

bool logSerialWriteAll(const uint8_t* data, const size_t len, const uint32_t budgetMs, const uint32_t stallMs) {
  const uint32_t startMs = millis();
  const LogSerialGuard guard(budgetMs);  // no log line lands between fragments
  if (!guard) return false;
  // No host attached: a short stall allowance only. Not zero, because the
  // HWCDC connected flag flaps for a moment after light sleep.
  const uint32_t stallLimitMs = logSerialHostConnected() ? stallMs : std::min<uint32_t>(stallMs, NO_HOST_STALL_MS);
  size_t sent = 0;
  uint32_t lastProgressMs = startMs;
  while (sent < len) {
    const size_t n = logSerial.write(data + sent, len - sent);
    sent += n;
    if (sent >= len) break;
    const uint32_t nowMs = millis();
    if (n > 0) lastProgressMs = nowMs;
    if (nowMs - startMs >= budgetMs || nowMs - lastProgressMs >= stallLimitMs) return false;
    delay(1);  // let the USB ISR drain the TX ring
  }
  return true;
}
#endif

std::string getLastLogs() {
  if (rtcLogMagic != LOG_RTC_MAGIC) {
    return {};
  }
  // Copy one line at a time under the lock and grow the string outside it:
  // string growth allocates, which must not happen with interrupts off.
  LOG_RING_LOCK();
  const size_t head = logHead % MAX_LOG_LINES;
  LOG_RING_UNLOCK();
  std::string output;
  char line[RTC_ENTRY_LEN];
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    const size_t idx = (head + i) % MAX_LOG_LINES;
    LOG_RING_LOCK();
    memcpy(line, logMessages[idx], RTC_ENTRY_LEN);
    LOG_RING_UNLOCK();
    line[RTC_ENTRY_LEN - 1] = '\0';
    if (line[0] != '\0') output.append(line, strnlen(line, RTC_ENTRY_LEN));
  }
  return output;
}

// Checks whether the RTC log state is consistent: rtcLogMagic must equal
// LOG_RTC_MAGIC and logHead must be in 0..MAX_LOG_LINES-1. Returns true if
// corruption is detected, in which case rtcLogMagic is still invalid and
// logMessages may contain garbage. Callers (e.g. HalSystem::begin on the
// panic-reboot path) must call clearLastLogs() after a true result to fully
// reinitialize the ring buffer and stamp the magic before getLastLogs() is used.
bool sanitizeLogHead() {
  if (rtcLogMagic != LOG_RTC_MAGIC || logHead >= MAX_LOG_LINES) {
    logHead = 0;
    return true;
  }
  return false;
}

void clearLastLogs() {
  LOG_RING_LOCK();
  for (size_t i = 0; i < MAX_LOG_LINES; i++) {
    logMessages[i][0] = '\0';
  }
  logHead = 0;
  rtcLogMagic = LOG_RTC_MAGIC;
  LOG_RING_UNLOCK();
}
