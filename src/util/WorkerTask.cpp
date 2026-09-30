#include "WorkerTask.h"

#ifndef SIMULATOR

#include <Arduino.h>
#include <Logging.h>
#include <PerfLog.h>
#include <TaskCores.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/task.h>

// Owners join before destruction; this only frees a parked PSRAM-stack task.
WorkerTask::~WorkerTask() { reap(); }

void WorkerTask::reap() {
  if (!parked || running()) return;
  // Suspends the parked task if it has not parked yet, deletes it, and frees
  // its PSRAM stack and internal TCB.
  vTaskDeleteWithCaps(static_cast<TaskHandle_t>(parked));
  parked = nullptr;
}

bool WorkerTask::start(const Fn taskFn, void* const taskCtx, const uint32_t stackBytes, const char* name,
                       const bool uiCore, const Stack stack) {
  if (running()) return false;
  reap();
  fn = taskFn;
  ctx = taskCtx;
  exitName = name;
  const BaseType_t core = uiCore ? TaskCores::kUi : TaskCores::kWorker;
  active.store(true, std::memory_order_release);
  if (stack == Stack::Psram) {
    // Set before the task can run: entry() reads it to decide how to exit.
    psram = true;
    // TCB stays internal (IDF rule); only the stack goes to PSRAM.
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCoreWithCaps(&entry, name, stackBytes, this, 1, &handle, core,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
      parked = handle;
      return true;
    }
    LOG_DBG("TASK", "%s: no PSRAM stack, using internal RAM", name);
  }
  psram = false;
  if (xTaskCreatePinnedToCore(&entry, name, stackBytes, this, 1, nullptr, core) != pdPASS) {
    active.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

bool WorkerTask::join(const uint32_t timeoutMs) {
  const uint32_t startMs = millis();
  while (running()) {
    if (millis() - startMs >= timeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  reap();
  return true;
}

void WorkerTask::entry(void* const self) {
  auto* task = static_cast<WorkerTask*>(self);
  task->fn(task->ctx);
  PerfLog::noteTaskExit(task->exitName);
  const bool parks = task->psram;
  // Last touch of the owner: after this store it may be destroyed (a PSRAM
  // task's owner joins first, which deletes this task).
  task->active.store(false, std::memory_order_release);
  if (parks) {
    // vTaskDeleteWithCaps from the owner frees the stack; waiting here keeps
    // this task off the CPU until then.
    for (;;) vTaskSuspend(nullptr);
  }
  vTaskDelete(nullptr);
}

#else  // SIMULATOR: no background network jobs.

WorkerTask::~WorkerTask() = default;
bool WorkerTask::start(Fn, void*, uint32_t, const char*, bool, Stack) { return false; }
bool WorkerTask::join(uint32_t) { return true; }
void WorkerTask::entry(void*) {}
void WorkerTask::reap() {}

#endif
