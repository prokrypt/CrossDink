#include "DictionaryLookupWorker.h"

#include <Logging.h>
#include <TaskCores.h>
#ifndef SIMULATOR
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#endif

#include "DictionaryLookupController.h"

DictionaryLookupWorker& DictionaryLookupWorker::instance() {
  static DictionaryLookupWorker worker;
  return worker;
}

bool DictionaryLookupWorker::start(DictionaryLookupController& owner) {
  if (taskHandle_ == nullptr) {
#ifdef SIMULATOR
    xTaskCreatePinnedToCore(taskEntry, "DictLookup", kStackBytes, this, 1, &taskHandle_, TaskCores::kWorker);
#else
    // Worker core: a lookup scans dictionary files on SD and must not
    // time-slice with the render task. SD only, never flash, so the stack can
    // live in PSRAM; internal RAM is the fallback. The task is never deleted.
    if (xTaskCreatePinnedToCoreWithCaps(taskEntry, "DictLookup", kStackBytes, this, 1, &taskHandle_, TaskCores::kWorker,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
      taskHandle_ = nullptr;
      xTaskCreatePinnedToCore(taskEntry, "DictLookup", kStackBytes, this, 1, &taskHandle_, TaskCores::kWorker);
    }
#endif
    if (taskHandle_ == nullptr) {
      LOG_ERR("DICT", "Could not start dictionary lookup worker");
      return false;
    }
  }

  DictionaryLookupController* expected = nullptr;
  if (!owner_.compare_exchange_strong(expected, &owner, std::memory_order_release, std::memory_order_relaxed)) {
    LOG_ERR("DICT", "Dictionary lookup worker is busy");
    return false;
  }

  xTaskNotify(taskHandle_, 1, eIncrement);
  return true;
}

void DictionaryLookupWorker::waitForOwner(const DictionaryLookupController& owner) {
  while (owner_.load(std::memory_order_acquire) == &owner) vTaskDelay(1);
}

void DictionaryLookupWorker::taskEntry(void* context) { static_cast<DictionaryLookupWorker*>(context)->run(); }

void DictionaryLookupWorker::run() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    auto* owner = owner_.load(std::memory_order_acquire);
    if (owner == nullptr) continue;
    owner->runLookup();
    owner_.store(nullptr, std::memory_order_release);
  }
}
