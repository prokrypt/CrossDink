#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef SIMULATOR
#include <freertos/idf_additions.h>
#endif

// Short-lived helper tasks with their stack in PSRAM. An internal-RAM stack
// that deletes itself is freed later by the idle task, so allocations made
// while it ran sit behind an 8 KB hole and the largest internal block shrinks
// for the rest of the session. Only jobs that never write flash may use one
// (these touch the SD card only). The worker parks once done; the joiner
// deletes it, which frees the stack.
namespace psramworker {

inline bool start(TaskFunction_t fn, const char* name, const uint32_t stackBytes, void* arg, TaskHandle_t* task,
                  const BaseType_t core) {
#ifdef SIMULATOR
  return xTaskCreatePinnedToCore(fn, name, stackBytes, arg, 1, task, core) == pdPASS;
#else
  return xTaskCreatePinnedToCoreWithCaps(fn, name, stackBytes, arg, 1, task, core,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
#endif
}

[[noreturn]] inline void park() {
#ifdef SIMULATOR
  vTaskDelete(nullptr);
#endif
  for (;;) vTaskSuspend(nullptr);
}

inline void reap(TaskHandle_t& task) {
#ifndef SIMULATOR
  if (task) vTaskDeleteWithCaps(task);
#endif
  task = nullptr;
}

}  // namespace psramworker
