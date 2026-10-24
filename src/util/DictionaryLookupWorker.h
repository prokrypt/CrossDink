#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

class DictionaryLookupController;

// A single dictionary worker, created on the first lookup and kept parked,
// avoids allocating a 4 KB task stack for every lookup; the stack is in PSRAM.
// Controllers still own all lookup state and must wait for the worker before
// they are destroyed.
class DictionaryLookupWorker {
 public:
  static DictionaryLookupWorker& instance();

  // Starts a lookup for owner. Returns false when the static task could not be
  // created or another controller still owns the worker.
  bool start(DictionaryLookupController& owner);

  // Waits until owner is no longer running on the worker. The owner must set
  // its cancellation flag before calling this method.
  void waitForOwner(const DictionaryLookupController& owner);

 private:
  static constexpr size_t kStackBytes = 4096;

  static void taskEntry(void* context);
  void run();

  TaskHandle_t taskHandle_ = nullptr;
  std::atomic<DictionaryLookupController*> owner_{nullptr};
};
