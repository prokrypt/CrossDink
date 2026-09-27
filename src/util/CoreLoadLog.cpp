#include "CoreLoadLog.h"

#if CROSSINK_CORE_LOAD_LOG

#include <Logging.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdio>

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

TaskStatus_t statuses[kMaxTasks];
configRUN_TIME_COUNTER_TYPE deltas[kMaxTasks];
PreviousRunTime previous[kMaxTasks];
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
}  // namespace

namespace CoreLoadLog {

void logSinceLast() {
  const UBaseType_t count = uxTaskGetSystemState(statuses, kMaxTasks, nullptr);
  if (count == 0) {
    LOG_ERR("CPU", "More than %u tasks; core load not sampled", static_cast<unsigned>(kMaxTasks));
    return;
  }
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

  LOG_INF("CPU", "core0 %u%% core1 %u%% over %lu ms |%s", static_cast<unsigned>(loadPct[0]),
          static_cast<unsigned>(loadPct[1]), static_cast<unsigned long>(elapsedUs / 1000), top);
}

}  // namespace CoreLoadLog

#endif
