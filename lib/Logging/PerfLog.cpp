#include "PerfLog.h"

#if CROSSDINK_PERF_LOG && !defined(SIMULATOR)

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_rtc_time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#if CONFIG_PM_PROFILING
#include <esp_pm.h>
#include <esp_sleep.h>
#include <soc/rtc_cntl_reg.h>
#endif

// [PM] needs x4-pro-debug's custom_sdkconfig. Bins built against a framework
// compiled without it lost every [PM] line from 2c6751af to ce70db10 with no
// build error: fail the build instead.
#if CROSSDINK_REQUIRE_PM_PROFILING && !(CONFIG_PM_PROFILING && CONFIG_PM_LIGHT_SLEEP_CALLBACKS)
#error \
    "framework built without CONFIG_PM_PROFILING/CONFIG_PM_LIGHT_SLEEP_CALLBACKS: delete sdkconfig.defaults and rebuild"
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
volatile uint32_t sampleSeq = 0;
// An input with no render start this long after it drew nothing (ignored
// presses); a later render belongs to something else. Slow first renders
// (opening a book with a long section build) stay well under this.
constexpr uint32_t NO_RENDER_MS = 15000;
std::atomic<uint32_t> inputSeq{0};
bool firstInkLogged = false;
// Last rendered activity (render task writes, main loop reads; a torn read
// only mislabels one debug line) and the one the open [PM] window belongs to.
char currentAct[24] = "-";
char pmWindowAct[24] = "-";
WakeCountFn wakeCounter = nullptr;
PmWindowFn pmWindowHook = nullptr;
WakePinsFn wakePinsFn = nullptr;
std::atomic<uint32_t> loopPasses{0};

// Boot phase marks (setup() order), printed with the first ink.
constexpr int BOOT_PHASES = 8;
const char* bootPhaseName[BOOT_PHASES];
uint32_t bootPhaseMs[BOOT_PHASES];
int bootPhaseCount = 0;

constexpr uint32_t RESTART_MAGIC = 0x52535431;  // "RST1"
RTC_NOINIT_ATTR uint32_t restartMagic;
RTC_NOINIT_ATTR uint64_t restartRequestedUs;

constexpr uint32_t SLEEP_MAGIC = 0x534C5031;  // "SLP1"
RTC_NOINIT_ATTR uint32_t sleepMagic;
RTC_NOINIT_ATTR uint64_t sleepEnteredUs;
RTC_NOINIT_ATTR uint32_t sleepAwakeMs;
RTC_NOINIT_ATTR char sleepReason[20];
RTC_NOINIT_ATTR char sleepActivity[24];

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
uint32_t pmLastRejectCause = 0;  // RTC_CNTL reject-cause bits of the last rejected sleep, 0 = none seen

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// Light-sleep exits per wake cause (esp_sleep_get_wakeup_causes bitmap), from
// the PM exit hook: the idle task, flash code, cache on.
std::atomic<uint32_t> wakeTimer{0}, wakeGpio{0}, wakeWifi{0}, wakeOther{0};
esp_err_t countWake(const int64_t sleptUs, void*) {
  if (sleptUs <= 0) return ESP_OK;  // the sleep was skipped or rejected
  const uint32_t causes = esp_sleep_get_wakeup_causes();
  if (causes & BIT(ESP_SLEEP_WAKEUP_TIMER)) add(wakeTimer, 1);
  if (causes & BIT(ESP_SLEEP_WAKEUP_GPIO)) add(wakeGpio, 1);
  if (causes & BIT(ESP_SLEEP_WAKEUP_WIFI)) add(wakeWifi, 1);
  if (causes & ~(BIT(ESP_SLEEP_WAKEUP_TIMER) | BIT(ESP_SLEEP_WAKEUP_GPIO) | BIT(ESP_SLEEP_WAKEUP_WIFI)))
    add(wakeOther, 1);
  return ESP_OK;
}
#endif

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
  char held[96] = "";  // locks held at this dump, the per-core rtos ones left out
  size_t heldLen = 0;
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
        if (active > 0 && strncmp(name, "rtos", 4) != 0 && heldLen < sizeof(held)) {
          const int n = snprintf(held + heldLen, sizeof(held) - heldLen, "%s%s", heldLen ? "," : "", name);
          if (n > 0) heldLen += static_cast<size_t>(n);
        }
      }
      continue;
    }
    // Mode rows: "SLEEP     80 M        23547275    77%" (the frequency may be "240M").
    // Match the M after the number: "CPU_MAX" itself contains an M.
    long long us = 0;
    if (sscanf(line, "%15s %*d M %lld", name, &us) != 2) continue;
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
  char causeText[64] = "";
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  // Sleep exits by cause (several can share one exit).
  snprintf(causeText, sizeof(causeText), " cause=t:%lu g:%lu w:%lu o:%lu", static_cast<unsigned long>(take(wakeTimer)),
           static_cast<unsigned long>(take(wakeGpio)), static_cast<unsigned long>(take(wakeWifi)),
           static_cast<unsigned long>(take(wakeOther)));
#endif
#ifdef RTC_CNTL_SLP_REJECT_CAUSE_REG
  // The last rejected sleep's cause, in wakeup-trigger bits (S3: 0 ext0, 1 ext1,
  // 2 GPIO, 3 timer, 5 Wi-Fi, 6/7 UART, 8 touch).
  if (rejects != pmPrevRejects) {
    pmLastRejectCause = REG_READ(RTC_CNTL_SLP_REJECT_CAUSE_REG) & RTC_CNTL_REJECT_CAUSE;
    const size_t used = strlen(causeText);
    snprintf(causeText + used, sizeof(causeText) - used, " rjc=0x%lx", static_cast<unsigned long>(pmLastRejectCause));
  }
#endif
  LOG_DBG("PM",
          "%lus: act=%s sleep=%u%% cpumax=%u%% ls=%ld rej=%ld wake=gpio:%ld(btn %lu,touch %lu) timer:%ld%s loop=%lu "
          "top=%s",
          static_cast<unsigned long>(windowUs / 1000000), act, pmPct(sleepUs - pmPrevSleepUs, windowUs),
          pmPct(cpuMaxUs - pmPrevCpuMaxUs, windowUs), lsWindow, rejects - pmPrevRejects, gpioWakes,
          static_cast<unsigned long>(wakeButtons), static_cast<unsigned long>(wakeTouch),
          lsWindow > gpioWakes ? lsWindow - gpioWakes : 0L, causeText, static_cast<unsigned long>(take(loopPasses)),
          topText);
  // Every attempt rejected: name what could be doing it, at most once a minute.
  // A wake pin whose level now equals its armed level rejects every sleep.
  static long long lastRejectDiagUs = 0;
  if (lsWindow == 0 && rejects != pmPrevRejects && (lastRejectDiagUs == 0 || bootUs - lastRejectDiagUs >= 60000000)) {
    lastRejectDiagUs = bootUs;
    char pins[160] = "-";
    if (wakePinsFn) wakePinsFn(pins, sizeof(pins));
    LOG_INF("PM", "all sleeps rejected: wake pins (armed/now) %s | locks held %s | usb host %d", pins,
            held[0] ? held : "-", logSerialHostConnected() ? 1 : 0);
  }
  if (pmWindowHook) {
    // IDF's per-core locks: held while that core is out of its idle task.
    static const char* const kRtosLock[2] = {"rtos0", "rtos1"};
    unsigned rtosPct[2] = {0, 0};
    for (int i = 0; i < lockCount; i++) {
      for (int core = 0; core < 2; core++) {
        if (strcmp(locks[i].name, kRtosLock[core]) == 0)
          rtosPct[core] = pmPct(locks[i].us - pmPrevLockUs(locks[i].name), windowUs);
      }
    }
    pmWindowHook(rtosPct[0], rtosPct[1], gpioWakes);
  }

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

bool lightSleepStats(LightSleepStats& out) {
#if CONFIG_PM_PROFILING
  // The totals at the last [PM] window (main loop); a torn read from another
  // task only skews one displayed number.
  if (pmPrevBootUs <= 0) return false;  // no window yet
  out.sleeps = static_cast<uint32_t>(pmPrevSleeps);
  out.rejects = static_cast<uint32_t>(pmPrevRejects);
  out.upS = static_cast<uint32_t>(pmPrevBootUs / 1000000);
  out.sleepPct = static_cast<uint8_t>(pmPct(pmPrevSleepUs, pmPrevBootUs));
  out.rejectCause = pmLastRejectCause;
  // Lowest set wakeup-trigger bit (S3 numbering, as rjc= in [PM]).
  static const char* const kCause[] = {"ext0", "ext1", "GPIO", "timer", "SDIO", "Wi-Fi", "UART0", "UART1", "touch"};
  out.rejectCauseName = "-";
  for (unsigned i = 0; i < sizeof(kCause) / sizeof(kCause[0]); i++) {
    if (pmLastRejectCause & (1u << i)) {
      out.rejectCauseName = kCause[i];
      break;
    }
  }
  return true;
#else
  (void)out;
  return false;
#endif
}

uint32_t nextInputSeq() { return inputSeq.fetch_add(1, std::memory_order_relaxed) + 1; }

void noteInput(const bool release, const char* kind, const uint32_t seq) {
  if (!kind) kind = "-";
  if (inputPending) {
    if (release && renderStartMs != 0) return;
    // Finger moves before a tap or swipe lands, or while a refresh runs, must
    // not steal the sample of the action already waiting for ink.
    if (strcmp(kind, "touch") == 0) return;
    // A replaced sample still gets its line, so every #N is accounted for. A
    // release restarting its own press, and bare contact moves, are not actions.
    if (strcmp(inputKind, "touch") != 0 && !release) {
      LOG_DBG("LAT", "#%lu in=%s act=%s superseded by #%lu", static_cast<unsigned long>(sampleSeq), inputKind,
              renderStartMs != 0 ? renderActivity : "-", static_cast<unsigned long>(seq));
    }
  }
  sampleSeq = seq;
  inputKind = kind;
  renderStartMs = 0;
  renderEndMs = 0;
  pagePath = "-";
  inputMs = millis();
  inputPending = true;
}

void noteRenderStart(const char* activity) {
  snprintf(currentAct, sizeof(currentAct), "%s", activity ? activity : "-");
  if (inputPending && renderStartMs == 0 && millis() - inputMs > NO_RENDER_MS) {
    // Nothing drew for the input; do not bill this unrelated render to it.
    inputPending = false;
    if (strcmp(inputKind, "touch") != 0) {
      LOG_DBG("LAT", "#%lu in=%s act=- no render", static_cast<unsigned long>(sampleSeq), inputKind);
    }
  }
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
  // No render since the input: this ink is a frame drawn before it (a deferred
  // refresh ending, or finished by the next display call). Keep waiting.
  if (renderStartMs == 0) return;
  inputPending = false;
  // The keyboard logs its own per-keystroke key-to-ink ([KBD] prev_key_to_ink);
  // [LAT] drops samples there when strokes overlap.
  if (strcmp(renderActivity, "KeyboardEntry") == 0) return;
  const uint32_t in = inputMs;
  const uint32_t rs = renderStartMs;
  // A blocking refresh finishes inside render(), before its end is noted.
  const uint32_t re = renderEndMs != 0 ? renderEndMs : now;
  LOG_DBG("LAT", "#%lu in=%s act=%s path=%s queue=%lu render=%lu ink=%lu total=%lu",
          static_cast<unsigned long>(sampleSeq), inputKind, renderActivity, pagePath,
          static_cast<unsigned long>(rs - in), static_cast<unsigned long>(re - rs),
          static_cast<unsigned long>(now - re), static_cast<unsigned long>(now - in));
}

void noteDeepSleep(const char* reason, const char* activity) {
  snprintf(sleepReason, sizeof(sleepReason), "%s", reason ? reason : "-");
  snprintf(sleepActivity, sizeof(sleepActivity), "%s", activity ? activity : "-");
  sleepAwakeMs = millis();
  sleepEnteredUs = esp_rtc_get_time_us();
  sleepMagic = SLEEP_MAGIC;
  LOG_DBG("SLP", "deep sleep: reason=%s act=%s awake=%lu s", sleepReason, sleepActivity,
          static_cast<unsigned long>(sleepAwakeMs / 1000));
}

void logLastSleep() {
  if (sleepMagic != SLEEP_MAGIC) {
    LOG_INF("BOOT", "last sleep: none recorded (cold boot, power loss or restart)");
    return;
  }
  sleepMagic = 0;
  sleepReason[sizeof(sleepReason) - 1] = '\0';
  sleepActivity[sizeof(sleepActivity) - 1] = '\0';
  // RTC time keeps counting through deep sleep; this span also covers the boot
  // up to this line.
  const uint64_t sleptUs = esp_rtc_get_time_us() - sleepEnteredUs;
  LOG_INF("BOOT", "last sleep: reason=%s act=%s awake=%lu s slept=%lu s", sleepReason, sleepActivity,
          static_cast<unsigned long>(sleepAwakeMs / 1000), static_cast<unsigned long>(sleptUs / 1000000ULL));
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
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  static bool hooked = false;
  if (!hooked) {
    hooked = true;
    esp_pm_sleep_cbs_register_config_t cbs = {};
    cbs.exit_cb = countWake;
    if (esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK) LOG_ERR("PM", "sleep exit hook failed: no cause= counts");
  }
#endif
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
void setWakePinsDescriber(const WakePinsFn fn) { wakePinsFn = fn; }

void noteTaskExit(const char* name) {
  // Workers exit on either core; a short critical section guards the table.
  struct Low {
    const char* name;
    uint32_t minFree;
  };
  static Low lows[12];
  static portMUX_TYPE lowsMux = portMUX_INITIALIZER_UNLOCKED;
  const uint32_t freeBytes = uxTaskGetStackHighWaterMark(nullptr);  // bytes on ESP-IDF
  bool newLow = false;
  taskENTER_CRITICAL(&lowsMux);
  for (auto& low : lows) {
    if (low.name == nullptr || strcmp(low.name, name) == 0) {
      newLow = low.name == nullptr || freeBytes < low.minFree;
      if (newLow) low = {name, freeBytes};
      break;
    }
  }
  taskEXIT_CRITICAL(&lowsMux);
  if (newLow) {
    LOG_DBG("STK", "exit %s: min free %lu", name, static_cast<unsigned long>(freeBytes));
  }
}

void noteLoopPass() { add(loopPasses, 1); }

void setPmWindowHook(const PmWindowFn fn) { pmWindowHook = fn; }

}  // namespace PerfLog

#endif
