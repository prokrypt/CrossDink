#pragma once

#include <atomic>
#include <cstdint>

// One background job at a time on the worker core (also the Wi-Fi core), at
// the loop/render priority. The owner keeps the WorkerTask as a member: the
// task's last touch of it is clearing running(), so once running() reads false
// the owner may be destroyed (with a PSRAM stack: after join()).
class WorkerTask {
 public:
  using Fn = void (*)(void* ctx);
  enum class Stack : uint8_t { Internal, Psram };

  WorkerTask() = default;
  ~WorkerTask();
  WorkerTask(const WorkerTask&) = delete;
  WorkerTask& operator=(const WorkerTask&) = delete;

  // Runs fn(ctx) on a new task. The default stack is internal RAM. A PSRAM
  // stack saves internal RAM but is unsafe while the flash cache is off, so
  // only jobs that never write flash (no NVS, OTA, SD-to-flash) may use it;
  // it falls back to internal RAM when PSRAM is short. uiCore pins the task to
  // the render core instead (at the same priority, so they time-slice). False
  // when a job is still running or the task could not start.
  bool start(Fn fn, void* ctx, uint32_t stackBytes, const char* name, bool uiCore = false,
             Stack stack = Stack::Internal);
  bool running() const { return active.load(std::memory_order_acquire); }
  // Blocks the caller until the task has exited or timeoutMs passed; false on
  // timeout. An owner that gives up must outlive the task (static storage).
  bool join(uint32_t timeoutMs = UINT32_MAX);

 private:
  static void entry(void* self);
  // Deletes a finished PSRAM-stack task and frees its stack (such a task
  // cannot free its own stack, so it parks after clearing running()).
  void reap();

  Fn fn = nullptr;
  void* parked = nullptr;  // TaskHandle_t of a PSRAM-stack task
  bool psram = false;
  void* ctx = nullptr;
  const char* exitName = nullptr;  // a literal: the exit log keeps the pointer
  std::atomic<bool> active{false};
};
