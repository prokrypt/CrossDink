#include "WorkerTask.h"

#ifndef SIMULATOR

#include <Arduino.h>
#include <PerfLog.h>
#include <TaskCores.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

bool WorkerTask::start(const Fn taskFn, void* const taskCtx, const uint32_t stackBytes, const char* name) {
  if (running()) return false;
  fn = taskFn;
  ctx = taskCtx;
  exitName = name;
  active.store(true, std::memory_order_release);
  if (xTaskCreatePinnedToCore(&entry, name, stackBytes, this, 1, nullptr, TaskCores::kWorker) != pdPASS) {
    active.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

bool WorkerTask::join(const uint32_t timeoutMs) const {
  const uint32_t startMs = millis();
  while (running()) {
    if (millis() - startMs >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

void WorkerTask::entry(void* const self) {
  auto* task = static_cast<WorkerTask*>(self);
  task->fn(task->ctx);
  PerfLog::noteTaskExit(task->exitName);
  // Last touch of the owner: after this store it may be destroyed.
  task->active.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

#else  // SIMULATOR: no background network jobs.

bool WorkerTask::start(Fn, void*, uint32_t, const char*) { return false; }
bool WorkerTask::join(uint32_t) const { return true; }
void WorkerTask::entry(void*) {}

#endif
