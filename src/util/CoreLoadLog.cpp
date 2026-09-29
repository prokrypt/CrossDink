#include "CoreLoadLog.h"

#if CROSSDINK_CORE_LOAD_LOG && !defined(SIMULATOR)

#include <sdkconfig.h>

#if !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS || !CONFIG_FREERTOS_USE_TRACE_FACILITY
#error "CROSSDINK_CORE_LOAD_LOG needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS and CONFIG_FREERTOS_USE_TRACE_FACILITY"
#endif
// Deltas are compared against esp_timer_get_time(), so the counter must tick in
// microseconds. With the default 32-bit counter it wraps every ~71 minutes;
// unsigned subtraction keeps deltas right while samples are closer than that.
#if !CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER
#error "CROSSDINK_CORE_LOAD_LOG needs CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER (microsecond counter)"
#endif

#include <Arduino.h>
#include <Logging.h>
#include <PerfLog.h>
#include <esp_attr.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {
// Static so sampling never touches the heap; debug builds only.
constexpr UBaseType_t kMaxTasks = 48;
constexpr int kTopTasks = 5;
// Skip the line when both cores were nearly idle, so an idle device stays quiet.
constexpr uint32_t kMinLoggedLoadPct = 5;

struct PreviousRunTime {
  TaskHandle_t handle;
  configRUN_TIME_COUNTER_TYPE runTime;
};

EXT_RAM_NOINIT_ATTR TaskStatus_t statuses[kMaxTasks];
EXT_RAM_NOINIT_ATTR configRUN_TIME_COUNTER_TYPE deltas[kMaxTasks];
EXT_RAM_NOINIT_ATTR PreviousRunTime previous[kMaxTasks];
UBaseType_t previousCount = 0;
int64_t previousSampleUs = 0;

// A task first seen this interval has no baseline; count none of its time.
configRUN_TIME_COUNTER_TYPE previousRunTimeOf(const TaskStatus_t& status) {
  for (UBaseType_t i = 0; i < previousCount; i++) {
    if (previous[i].handle == status.xHandle) return previous[i].runTime;
  }
  return status.ulRunTimeCounter;
}

char coreTag(const TaskHandle_t handle) {
  const BaseType_t core = xTaskGetCoreID(handle);
  return core == 0 ? '0' : core == 1 ? '1' : '-';
}

// Stack headroom changes slowly; log it on the first sample and then about
// every 30 s (the caller samples every 2 s).
constexpr uint32_t kStackLogEverySamples = 15;
constexpr int kLowestStacks = 8;
constexpr unsigned kStackWarnBytes = 1536;  // loop/render tasks: log an error below this
uint32_t samplesSinceStackLog = kStackLogEverySamples;

// The tasks closest to overflowing: minimum free stack ever (bytes on ESP-IDF,
// whose StackType_t is one byte), lowest first.
void logLowestStackHeadroom(const UBaseType_t count) {
  if (++samplesSinceStackLog < kStackLogEverySamples) return;
  samplesSinceStackLog = 0;

  bool logged[kMaxTasks] = {};
  char line[240];
  size_t used = 0;
  line[0] = '\0';
  for (int rank = 0; rank < kLowestStacks; rank++) {
    int lowest = -1;
    for (UBaseType_t i = 0; i < count; i++) {
      if (logged[i]) continue;
      if (lowest < 0 || statuses[i].usStackHighWaterMark < statuses[lowest].usStackHighWaterMark) {
        lowest = static_cast<int>(i);
      }
    }
    if (lowest < 0) break;
    logged[lowest] = true;
    const int written = snprintf(line + used, sizeof(line) - used, " %s:%u", statuses[lowest].pcTaskName,
                                 static_cast<unsigned>(statuses[lowest].usStackHighWaterMark));
    if (written < 0 || static_cast<size_t>(written) >= sizeof(line) - used) break;
    used += static_cast<size_t>(written);
  }
  // The loop and render tasks always, whatever their rank: their sizes are
  // set by this app (loopTask via getArduinoLoopTaskStackSize(), the render
  // task by ActivityManager::begin), so these are the numbers to tune them by.
  used = 0;
  char watched[96];
  watched[0] = '\0';
  for (UBaseType_t i = 0; i < count; i++) {
    const char* name = statuses[i].pcTaskName;
    const bool isLoop = strcmp(name, "loopTask") == 0;
    if (!isLoop && strncmp(name, "ActivityManager", 15) != 0) continue;
    const unsigned freeBytes = static_cast<unsigned>(statuses[i].usStackHighWaterMark);
    const int written = isLoop ? snprintf(watched + used, sizeof(watched) - used, " loopTask:%u/%u", freeBytes,
                                          static_cast<unsigned>(getArduinoLoopTaskStackSize()))
                               : snprintf(watched + used, sizeof(watched) - used, " render:%u", freeBytes);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(watched) - used) break;
    used += static_cast<size_t>(written);
    if (freeBytes < kStackWarnBytes) LOG_ERR("STK", "%s has only %u bytes of stack left", name, freeBytes);
  }
  LOG_INF("STK", "lowest free stack (bytes):%s |%s", line, watched);
}
}  // namespace

namespace CoreLoadLog {

void logSinceLast() {
  const UBaseType_t count = uxTaskGetSystemState(statuses, kMaxTasks, nullptr);
  if (count == 0) {
    LOG_ERR("CPU", "More than %u tasks; core load not sampled", static_cast<unsigned>(kMaxTasks));
    return;
  }
  logLowestStackHeadroom(count);
  const int64_t nowUs = esp_timer_get_time();
  const int64_t elapsedUs = nowUs - previousSampleUs;
  const bool havePrevious = previousSampleUs != 0 && elapsedUs > 0;

  // Turn cumulative counters into per-interval deltas.
  configRUN_TIME_COUNTER_TYPE idleDelta[2] = {0, 0};
  const TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
  const TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
  for (UBaseType_t i = 0; i < count; i++) {
    deltas[i] = statuses[i].ulRunTimeCounter - previousRunTimeOf(statuses[i]);
    if (statuses[i].xHandle == idle0) idleDelta[0] = deltas[i];
    if (statuses[i].xHandle == idle1) idleDelta[1] = deltas[i];
  }
  for (UBaseType_t i = 0; i < count; i++) {
    previous[i] = {statuses[i].xHandle, statuses[i].ulRunTimeCounter};
  }
  previousCount = count;
  previousSampleUs = nowUs;
  if (!havePrevious) return;

  uint32_t loadPct[2];
  for (int core = 0; core < 2; core++) {
    const int64_t busyUs = std::max<int64_t>(0, elapsedUs - static_cast<int64_t>(idleDelta[core]));
    loadPct[core] = static_cast<uint32_t>(std::min<int64_t>(100, busyUs * 100 / elapsedUs));
  }
  if (loadPct[0] < kMinLoggedLoadPct && loadPct[1] < kMinLoggedLoadPct) return;

  // Busiest non-idle tasks, percent of one core over the interval.
  char top[200];
  size_t used = 0;
  top[0] = '\0';
  for (int rank = 0; rank < kTopTasks; rank++) {
    int best = -1;
    for (UBaseType_t i = 0; i < count; i++) {
      if (statuses[i].xHandle == idle0 || statuses[i].xHandle == idle1 || deltas[i] == 0) continue;
      if (best < 0 || deltas[i] > deltas[best]) best = static_cast<int>(i);
    }
    if (best < 0) break;
    const auto pct =
        static_cast<unsigned>(std::min<int64_t>(elapsedUs, static_cast<int64_t>(deltas[best])) * 100 / elapsedUs);
    const int written = snprintf(top + used, sizeof(top) - used, " %s(%c)%u%%", statuses[best].pcTaskName,
                                 coreTag(statuses[best].xHandle), pct);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(top) - used) break;
    used += static_cast<size_t>(written);
    deltas[best] = 0;
  }

  char act[24];
  PerfLog::currentActivity(act, sizeof(act));
  LOG_INF("CPU", "core0 %u%% core1 %u%% over %lu ms act=%s |%s", static_cast<unsigned>(loadPct[0]),
          static_cast<unsigned>(loadPct[1]), static_cast<unsigned long>(elapsedUs / 1000), act, top);
}

}  // namespace CoreLoadLog

#endif
