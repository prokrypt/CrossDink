#pragma once

#include <Arduino.h>
#include <BoardConfig.h>

#include <string>

/*
Define ENABLE_SERIAL_LOG to enable logging
Can be set in platformio.ini build_flags or as a compile definition

Define LOG_LEVEL to control log verbosity:
0 = ERR only
1 = ERR + INF
2 = ERR + INF + DBG
If not defined, defaults to 0

If you have a legitimate need for raw Serial access (e.g., binary data,
special formatting), use the underlying logSerial object directly:
    logSerial.printf("Special case: %d\n", value);
    logSerial.write(binaryData, length);

The logSerial reference (defined below) points to the board's physical USB
serial transport and won't trigger deprecation warnings.
*/

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif

static auto& logSerial = BoardConfig::serialTransport();
#define LOG_SERIAL_HAS_TX_TIMEOUT FREEINK_SERIAL_HAS_TX_TIMEOUT

void logPrintf(const char* level, const char* origin, const char* format, ...);

// Serializes logSerial writers so a protocol reply or binary stream is never
// split by a log line from another task. Recursive, so a holder may call
// logSerialWriteAll(). Log lines that can't get it within 2 ms skip the serial
// port (the RAM and PSRAM rings still get them). setup() calls logSerialInit()
// once; before that, and before the scheduler runs, writes go straight out.
void logSerialInit();
// The host-attached flag: true as soon as the raw HWCDC flag is, false only
// after it has stayed false for 1 s (it flaps every ~100-300 ms when idle).
bool logSerialHostConnected();
bool logSerialLock(uint32_t waitMs);
void logSerialUnlock();
class LogSerialGuard {
 public:
  explicit LogSerialGuard(uint32_t waitMs) : locked(logSerialLock(waitMs)) {}
  ~LogSerialGuard() {
    if (locked) logSerialUnlock();
  }
  LogSerialGuard(const LogSerialGuard&) = delete;
  LogSerialGuard& operator=(const LogSerialGuard&) = delete;
  explicit operator bool() const { return locked; }

 private:
  bool locked;
};

// Writes all of `data` to logSerial under logSerialLock, retrying for up to
// `budgetMs`. The log transport uses a 1 ms TX timeout, so a plain write loses
// bytes whenever the TX ring is full. Use this for protocol replies and binary
// streams a host waits on. Gives up early when nothing has gone out for
// `stallMs` (host stopped reading) or no host is attached. Returns false when
// not everything went out.
bool logSerialWriteAll(const uint8_t* data, size_t len, uint32_t budgetMs, uint32_t stallMs);
inline bool logSerialWriteAll(const char* data, size_t len, uint32_t budgetMs = 500, uint32_t stallMs = 50) {
  return logSerialWriteAll(reinterpret_cast<const uint8_t*>(data), len, budgetMs, stallMs);
}

#ifdef ENABLE_SERIAL_LOG
#if LOG_LEVEL >= 0
#define LOG_ERR(origin, format, ...) logPrintf("ERR", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_ERR(origin, format, ...)
#endif

#if LOG_LEVEL >= 1
#define LOG_INF(origin, format, ...) logPrintf("INF", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_INF(origin, format, ...)
#endif

#if LOG_LEVEL >= 2
#define LOG_DBG(origin, format, ...) logPrintf("DBG", origin, format "\n", ##__VA_ARGS__)
#else
#define LOG_DBG(origin, format, ...)
#endif
#else
#define LOG_DBG(origin, format, ...)
#define LOG_ERR(origin, format, ...)
#define LOG_INF(origin, format, ...)
#endif

std::string getLastLogs();
void clearLastLogs();
// Validates the RTC log state (magic word + logHead range). Returns true if
// corruption was detected (magic mismatch or logHead out of range), meaning
// logMessages is untrusted garbage. Callers should call clearLastLogs() when
// this returns true so getLastLogs() does not dump corrupt data into crash reports.
bool sanitizeLogHead();

class MySerialImpl : public Print {
 public:
  void begin(unsigned long baud) { logSerial.begin(baud); }

  // Support boolean conversion for compatibility with code like:
  //   if (Serial) or while (!Serial)
  operator bool() const { return logSerial; }

  __attribute__((deprecated("Use LOG_* macro instead"))) size_t printf(const char* format, ...);
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  void flush() override;
  static MySerialImpl instance;
};

#ifdef Serial
#undef Serial
#endif
#define Serial MySerialImpl::instance
