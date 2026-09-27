#include "PerfLog.h"

#if CROSSDINK_PERF_LOG && !defined(SIMULATOR)

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_rtc_time.h>
#include <sdkconfig.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#if CONFIG_PM_PROFILING
#include <esp_pm.h>
#endif

namespace PerfLog {
namespace {
// Counters are bumped from the main loop, the render task and background
// workers on both cores; relaxed atomics keep them exact without a lock.
std::atomic<uint32_t> sdOpens{0};
std::atomic<uint32_t> sdMisses{0};
std::atomic<uint32_t> sdReadBytes{0};
std::atomic<uint32_t> sdReadUs{0};
std::atomic<uint32_t> sdWriteBytes{0};
std::atomic<uint32_t> sdWriteUs{0};
std::atomic<uint32_t> imgHits{0};
std::atomic<uint32_t> imgMisses{0};
std::atomic<uint32_t> imgDecodeMs{0};

// Latency sample. Written by the main loop (input), the render task and the
// display busy-wait hook; a torn sample only mislabels one debug line.
volatile uint32_t inputMs = 0;
volatile uint32_t renderStartMs = 0;
volatile uint32_t renderEndMs = 0;
volatile bool inputPending = false;
// Copied: the activity can be destroyed before its refresh ends.
char renderActivity[24] = "-";
const char* volatile pagePath = "-";
const char* volatile inputKind = "-";
bool firstInkLogged = false;

constexpr uint32_t RESTART_MAGIC = 0x52535431;  // "RST1"
RTC_NOINIT_ATTR uint32_t restartMagic;
RTC_NOINIT_ATTR uint64_t restartRequestedUs;

uint32_t take(std::atomic<uint32_t>& counter) { return counter.exchange(0, std::memory_order_relaxed); }
void add(std::atomic<uint32_t>& counter, const uint32_t value) { counter.fetch_add(value, std::memory_order_relaxed); }

#if CONFIG_PM_PROFILING
// esp_pm_dump_locks() only writes to a FILE*; route it through a memory stream
// so each row becomes one [PM] log line that also reaches the log ring.
void logPmLocks() {
  static char dump[2048];
  FILE* stream = fmemopen(dump, sizeof(dump) - 1, "w");
  if (!stream) {
    LOG_ERR("PM", "fmemopen failed");
    return;
  }
  esp_pm_dump_locks(stream);
  const long used = ftell(stream);
  fclose(stream);
  dump[used > 0 && used < static_cast<long>(sizeof(dump)) ? used : 0] = '\0';
  char* save = nullptr;
  for (char* line = strtok_r(dump, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
    LOG_DBG("PM", "%s", line);
  }
}
#endif
}  // namespace

void noteInput(const bool release, const char* kind) {
  if (release && inputPending && renderStartMs != 0) return;
  inputKind = kind ? kind : "-";
  renderStartMs = 0;
  renderEndMs = 0;
  pagePath = "-";
  inputMs = millis();
  inputPending = true;
}

void noteRenderStart() {
  if (inputPending && renderStartMs == 0) renderStartMs = millis();
}

void noteRenderEnd(const char* activity) {
  if (inputPending && renderStartMs != 0 && renderEndMs == 0) {
    renderEndMs = millis();
    snprintf(renderActivity, sizeof(renderActivity), "%s", activity ? activity : "-");
  }
}

void notePagePath(const char* path) {
  if (inputPending) pagePath = path;
}

void noteInk() {
  const uint32_t now = millis();
  if (!firstInkLogged) {
    firstInkLogged = true;
    if (restartMagic == RESTART_MAGIC) {
      restartMagic = 0;
      const uint64_t acrossUs = esp_rtc_get_time_us() - restartRequestedUs;
      LOG_DBG("BOOT", "first ink at %lu ms; silent restart->ink %lu ms", static_cast<unsigned long>(now),
              static_cast<unsigned long>(acrossUs / 1000));
    } else {
      LOG_DBG("BOOT", "first ink at %lu ms", static_cast<unsigned long>(now));
    }
  }
  if (!inputPending) return;
  inputPending = false;
  const uint32_t in = inputMs;
  const uint32_t rs = renderStartMs;
  // A blocking refresh finishes inside render(), before its end is noted.
  const uint32_t re = renderEndMs != 0 ? renderEndMs : now;
  if (rs == 0) {
    LOG_DBG("LAT", "in=%s act=- total=%lu", inputKind, static_cast<unsigned long>(now - in));
    return;
  }
  LOG_DBG("LAT", "in=%s act=%s path=%s queue=%lu render=%lu ink=%lu total=%lu", inputKind, renderActivity, pagePath,
          static_cast<unsigned long>(rs - in), static_cast<unsigned long>(re - rs),
          static_cast<unsigned long>(now - re), static_cast<unsigned long>(now - in));
}

void noteRestart() {
  restartRequestedUs = esp_rtc_get_time_us();
  restartMagic = RESTART_MAGIC;
  LOG_DBG("RST", "silent restart at %lu ms", static_cast<unsigned long>(millis()));
}

void noteSdOpen(const bool opened) {
  add(sdOpens, 1);
  if (!opened) add(sdMisses, 1);
}

void noteSdRead(const uint32_t bytes, const uint32_t us) {
  add(sdReadBytes, bytes);
  add(sdReadUs, us);
}

void noteSdWrite(const uint32_t bytes, const uint32_t us) {
  add(sdWriteBytes, bytes);
  add(sdWriteUs, us);
}

void noteImage(const bool cacheHit, const uint32_t ms) {
  add(cacheHit ? imgHits : imgMisses, 1);
  if (!cacheHit) add(imgDecodeMs, ms);
}

void logPeriodic() {
  const uint32_t opens = take(sdOpens);
  const uint32_t misses = take(sdMisses);
  const uint32_t rb = take(sdReadBytes);
  const uint32_t rus = take(sdReadUs);
  const uint32_t wb = take(sdWriteBytes);
  const uint32_t wus = take(sdWriteUs);
  const uint32_t hits = take(imgHits);
  const uint32_t decodes = take(imgMisses);
  const uint32_t decodeMs = take(imgDecodeMs);
  if (opens | rb | wb | hits | decodes) {
    LOG_DBG("PERF", "sd open=%lu miss=%lu rd=%luKB/%lums wr=%luKB/%lums img hit=%lu dec=%lu/%lums",
            static_cast<unsigned long>(opens), static_cast<unsigned long>(misses),
            static_cast<unsigned long>(rb / 1024), static_cast<unsigned long>(rus / 1000),
            static_cast<unsigned long>(wb / 1024), static_cast<unsigned long>(wus / 1000),
            static_cast<unsigned long>(hits), static_cast<unsigned long>(decodes),
            static_cast<unsigned long>(decodeMs));
  }
#if CONFIG_PM_PROFILING
  static uint32_t lastPmMs = 0;
  if (millis() - lastPmMs >= 30000) {
    lastPmMs = millis();
    logPmLocks();
  }
#endif
}

}  // namespace PerfLog

#endif
