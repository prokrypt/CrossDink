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
// Last rendered activity (render task writes, main loop reads; a torn read
// only mislabels one debug line) and the one the open [PM] window belongs to.
char currentAct[24] = "-";
char pmWindowAct[24] = "-";
WakeCountFn wakeCounter = nullptr;

// Boot phase marks (setup() order), printed with the first ink.
constexpr int BOOT_PHASES = 8;
const char* bootPhaseName[BOOT_PHASES];
uint32_t bootPhaseMs[BOOT_PHASES];
int bootPhaseCount = 0;

constexpr uint32_t RESTART_MAGIC = 0x52535431;  // "RST1"
RTC_NOINIT_ATTR uint32_t restartMagic;
RTC_NOINIT_ATTR uint64_t restartRequestedUs;

uint32_t take(std::atomic<uint32_t>& counter) { return counter.exchange(0, std::memory_order_relaxed); }
void add(std::atomic<uint32_t>& counter, const uint32_t value) { counter.fetch_add(value, std::memory_order_relaxed); }

#if CONFIG_PM_PROFILING
// esp_pm_dump_locks() only writes cumulative-since-boot tables to a FILE*.
// Parse them from a memory stream and print one [PM] line with the change
// since the previous dump: light-sleep and CPU_MAX share of the window, sleep
// entries/rejects, and the three locks held longest.
struct PmLockTime {
  char name[16];
  int64_t us;
};
constexpr int PM_MAX_LOCKS = 16;
PmLockTime pmPrevLocks[PM_MAX_LOCKS];
int pmPrevLockCount = 0;
int64_t pmPrevBootUs = 0;
int64_t pmPrevSleepUs = 0;
int64_t pmPrevCpuMaxUs = 0;
long pmPrevSleeps = 0;
long pmPrevRejects = 0;

int64_t pmPrevLockUs(const char* name) {
  for (int i = 0; i < pmPrevLockCount; i++) {
    if (strcmp(pmPrevLocks[i].name, name) == 0) return pmPrevLocks[i].us;
  }
  return 0;
}

unsigned pmPct(const int64_t part, const int64_t whole) {
  return whole > 0 ? static_cast<unsigned>((part * 100 + whole / 2) / whole) : 0;
}

void logPmLocks(const char* act) {
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

  static PmLockTime locks[PM_MAX_LOCKS];  // main loop only; keeps 384 B off the stack
  int lockCount = 0;
  long long bootUs = 0;
  int64_t sleepUs = 0;
  int64_t cpuMaxUs = 0;
  long sleeps = 0;
  long rejects = 0;
  bool inModes = false;
  char* save = nullptr;
  for (char* line = strtok_r(dump, "\n", &save); line; line = strtok_r(nullptr, "\n", &save)) {
    if (sscanf(line, "Time since bootup: %lld", &bootUs) == 1) continue;
    if (strncmp(line, "Mode stats", 10) == 0) {
      inModes = true;
      continue;
    }
    if (sscanf(line, "light_sleep_counts:%ld light_sleep_reject_counts:%ld", &sleeps, &rejects) == 2) continue;
    char name[16];
    if (!inModes) {
      char type[16];
      int arg = 0, active = 0, count = 0;
      long long us = 0;
      if (lockCount < PM_MAX_LOCKS &&
          sscanf(line, "%15s %15s %d %d %d %lld", name, type, &arg, &active, &count, &us) == 6) {
        snprintf(locks[lockCount].name, sizeof(locks[lockCount].name), "%s", name);
        locks[lockCount].us = us;
        lockCount++;
      }
      continue;
    }
    // Mode rows: "SLEEP     80 M        23547275    77%" (the frequency may be "240M").
    const char* freqEnd = strchr(line, 'M');
    long long us = 0;
    if (sscanf(line, "%15s", name) != 1 || freqEnd == nullptr || sscanf(freqEnd + 1, "%lld", &us) != 1) continue;
    if (strcmp(name, "SLEEP") == 0) sleepUs = us;
    if (strcmp(name, "CPU_MAX") == 0) cpuMaxUs = us;
  }

  const int64_t windowUs = bootUs - pmPrevBootUs;
  // Top three locks by hold time in this window.
  int top[3] = {-1, -1, -1};
  int64_t topUs[3] = {0, 0, 0};
  for (int i = 0; i < lockCount; i++) {
    const int64_t d = locks[i].us - pmPrevLockUs(locks[i].name);
    for (int t = 0; t < 3; t++) {
      if (d > topUs[t]) {
        for (int k = 2; k > t; k--) {
          top[k] = top[k - 1];
          topUs[k] = topUs[k - 1];
        }
        top[t] = i;
        topUs[t] = d;
        break;
      }
    }
  }
  char topText[96] = "-";
  int pos = 0;
  for (int t = 0; t < 3 && top[t] >= 0; t++) {
    const int n = snprintf(topText + pos, sizeof(topText) - pos, "%s%s %u%%", t ? "," : "", locks[top[t]].name,
                           pmPct(topUs[t], windowUs));
    if (n < 0 || n >= static_cast<int>(sizeof(topText)) - pos) break;
    pos += n;
  }
  // Wake causes: GPIO line interrupts (they also fire while awake, so they
  // can exceed ls); the remaining light-sleep exits are timer wakes.
  uint32_t wakeButtons = 0;
  uint32_t wakeTouch = 0;
  if (wakeCounter) wakeCounter(wakeButtons, wakeTouch);
  const long lsWindow = sleeps - pmPrevSleeps;
  const long gpioWakes = static_cast<long>(wakeButtons + wakeTouch);
  LOG_DBG("PM", "%lus: act=%s sleep=%u%% cpumax=%u%% ls=%ld rej=%ld wake=gpio:%ld(btn %lu,touch %lu) timer:%ld top=%s",
          static_cast<unsigned long>(windowUs / 1000000), act, pmPct(sleepUs - pmPrevSleepUs, windowUs),
          pmPct(cpuMaxUs - pmPrevCpuMaxUs, windowUs), lsWindow, rejects - pmPrevRejects, gpioWakes,
          static_cast<unsigned long>(wakeButtons), static_cast<unsigned long>(wakeTouch),
          lsWindow > gpioWakes ? lsWindow - gpioWakes : 0L, topText);

  memcpy(pmPrevLocks, locks, sizeof(PmLockTime) * lockCount);
  pmPrevLockCount = lockCount;
  pmPrevBootUs = bootUs;
  pmPrevSleepUs = sleepUs;
  pmPrevCpuMaxUs = cpuMaxUs;
  pmPrevSleeps = sleeps;
  pmPrevRejects = rejects;
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

void noteRenderStart(const char* activity) {
  snprintf(currentAct, sizeof(currentAct), "%s", activity ? activity : "-");
  if (inputPending && renderStartMs == 0) {
    renderStartMs = millis();
    snprintf(renderActivity, sizeof(renderActivity), "%s", activity ? activity : "-");
  }
}

void noteRenderEnd() {
  if (inputPending && renderStartMs != 0 && renderEndMs == 0) {
    renderEndMs = millis();
  }
}

void notePagePath(const char* path) {
  if (inputPending) pagePath = path;
}

void noteBootPhase(const char* name) {
  if (firstInkLogged || bootPhaseCount >= BOOT_PHASES) return;
  bootPhaseName[bootPhaseCount] = name;
  bootPhaseMs[bootPhaseCount] = millis();
  bootPhaseCount++;
}

void noteInk() {
  const uint32_t now = millis();
  if (!firstInkLogged) {
    firstInkLogged = true;
    // "[BOOT] t start=54 sd=242 ... ink=1620 first_ink=2450": each phase is the
    // time since the previous mark; ink runs from the last mark to first ink.
    char phases[160];
    int pos = 0;
    uint32_t prev = 0;
    for (int i = 0; i < bootPhaseCount; i++) {
      const int n = snprintf(phases + pos, sizeof(phases) - pos, "%s=%lu ", bootPhaseName[i],
                             static_cast<unsigned long>(bootPhaseMs[i] - prev));
      if (n < 0 || n >= static_cast<int>(sizeof(phases)) - pos) break;
      pos += n;
      prev = bootPhaseMs[i];
    }
    phases[pos] = '\0';
    LOG_DBG("BOOT", "t %sink=%lu first_ink=%lu", phases, static_cast<unsigned long>(now - prev),
            static_cast<unsigned long>(now));
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
  // The keyboard logs its own per-keystroke key-to-ink ([KBD] prev_key_to_ink);
  // [LAT] drops samples there when strokes overlap.
  if (strcmp(renderActivity, "KeyboardEntry") == 0) return;
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
  char act[sizeof(currentAct)];
  currentActivity(act, sizeof(act));
  const bool activityChanged = strcmp(act, pmWindowAct) != 0;
  if (activityChanged || millis() - lastPmMs >= 30000) {
    lastPmMs = millis();
    // The window so far belongs to the activity it started in.
    logPmLocks(pmWindowAct);
    memcpy(pmWindowAct, act, sizeof(pmWindowAct));
  }
#endif
}

void currentActivity(char* out, const uint32_t size) {
  if (size == 0) return;
  snprintf(out, size, "%s", currentAct);
}

void setWakeCounter(const WakeCountFn fn) { wakeCounter = fn; }

}  // namespace PerfLog

#endif
