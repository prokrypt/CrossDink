#pragma once

#include <atomic>
#include <cstdint>

// One background job at a time on the worker core (also the Wi-Fi core), at
// the loop/render priority. The owner keeps the WorkerTask as a member: the
// task's last touch of it is clearing running(), so once running() reads false
// the owner may be destroyed.
class WorkerTask {
 public:
  using Fn = void (*)(void* ctx);

  // Runs fn(ctx) on a new task. The stack is internal RAM: Wi-Fi/TLS and SD
  // code run on it, and PSRAM stacks are unsafe while flash cache is off.
  // False when a job is still running or the task could not start.
  bool start(Fn fn, void* ctx, uint32_t stackBytes, const char* name);
  bool running() const { return active.load(std::memory_order_acquire); }
  // Blocks the caller until the task has exited or timeoutMs passed; false on
  // timeout. An owner that gives up must outlive the task (static storage).
  bool join(uint32_t timeoutMs = UINT32_MAX) const;

 private:
  static void entry(void* self);

  Fn fn = nullptr;
  void* ctx = nullptr;
  std::atomic<bool> active{false};
};
