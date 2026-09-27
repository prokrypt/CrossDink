#include "LibraryPrewarm.h"

#ifndef SIMULATOR
#include <Arduino.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <optional>

#include "CrossPointSettings.h"
#endif

namespace LibraryPrewarm {

#ifdef SIMULATOR

// The simulator's FreeRTOS shim is not relied on for background builds; the
// Library keeps scanning in the foreground there.
void tick(bool) {}
void pause() {}
void stop(bool) {}
bool active() { return false; }
bool working() { return false; }
bool finishForLibrary(uint32_t) { return true; }

#else

namespace {
// The builder normally runs on the 8 KB Arduino loop task; give it the same.
// Transient: the stack is freed as soon as the build has been collected.
constexpr uint32_t kStackBytes = 8192;
// Same internal-heap floor as other opportunistic background work: a paused
// build keeps its buffers, and Home must still be able to decode covers.
constexpr uint32_t kMinFreeHeap = 96 * 1024;
constexpr uint32_t kMinMaxAlloc = 48 * 1024;

// Everything below is touched by the main loop only, except the atomics and
// the result fields, which the build task writes before giving doneSem.
TaskHandle_t task = nullptr;
SemaphoreHandle_t doneSem = nullptr;
std::atomic<bool> paused{false};
std::atomic<bool> cancelRequested{false};
uint32_t startGeneration = 0;
bool useMetadata = false;
bool buildOk = false;
bool buildCancelled = false;
// Timing for the finish log, so a slow walk on a device can be told apart from
// one that was held off by Home. pausedMs is written by the build task only.
uint32_t startedAtMs = 0;
uint32_t pausedMs = 0;
library::BuildStats lastStats;
// Why the last tick() did not start a build, logged once per change.
enum class SkipReason : uint8_t { None, LowHeap };
SkipReason lastSkip = SkipReason::None;
// A build that failed on its own (not stopped) is not retried until the next
// boot; the Library still scans in the foreground and reports the problem.
bool gaveUp = false;
// Build task only. Held while the walk runs and dropped while it is paused:
// the main loop's idle branch otherwise leaves it at the lowest CPU clock.
std::optional<HalPowerManager::Lock> fullSpeed;

bool serviceBuild(void*) {
  // Blocks instead of polling so a paused build costs no wakeups and the chip
  // can stay in light sleep. resume/cancel send a notification.
  if (paused.load(std::memory_order_acquire) && !cancelRequested.load(std::memory_order_acquire)) {
    const uint32_t pausedAtMs = millis();
    fullSpeed.reset();
    while (paused.load(std::memory_order_acquire) && !cancelRequested.load(std::memory_order_acquire)) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    fullSpeed.emplace();
    pausedMs += millis() - pausedAtMs;
  }
  return !cancelRequested.load(std::memory_order_acquire);
}

void buildTask(void*) {
  library::BuildStats stats;
  library::BuildControl control;
  control.service = &serviceBuild;
  fullSpeed.emplace();
  buildOk = library::buildLibraryIndex("/", stats, useMetadata, &control);
  // Released before doneSem: the owner deletes this task once it takes it.
  fullSpeed.reset();
  buildCancelled = stats.cancelled;
  lastStats = stats;
  xSemaphoreGive(doneSem);
  // The owner deletes this task after collecting the result. Parking here
  // rather than self-deleting keeps a late notification from the owner safe.
  for (;;) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void wake() {
  if (task) xTaskNotifyGive(task);
}

void finalize() {
  vTaskDelete(task);
  task = nullptr;
  if (buildOk) {
    // The generation read before the walk: a change made during it leaves the
    // counter ahead, so the Library rescans. A degraded index (sort or dedup
    // allocation failed under Home's heap) is left for the Library's own
    // build, which runs with Home's buffers freed and can repair it.
    if (!lastStats.ranksDegraded && !lastStats.dedupDegraded) {
      Storage.noteLibraryScanned(startGeneration);
    } else {
      LOG_INF("LIBPW", "Background index is degraded; the Library will rebuild it");
    }
    LOG_INF("LIBPW", "Library index ready in the background: %u books, %u parsed, %ums total, %ums paused",
            static_cast<unsigned>(lastStats.books), static_cast<unsigned>(lastStats.parsed),
            static_cast<unsigned>(millis() - startedAtMs), static_cast<unsigned>(pausedMs));
  } else if (buildCancelled) {
    LOG_INF("LIBPW", "Background Library build stopped after %ums; will retry on Home",
            static_cast<unsigned>(millis() - startedAtMs));
  } else {
    gaveUp = true;
    LOG_ERR("LIBPW", "Background Library build failed; leaving it to the Library");
  }
}

void collectIfDone() {
  if (task && xSemaphoreTake(doneSem, 0) == pdTRUE) finalize();
}

void waitForDone() {
  if (!task) return;
  xSemaphoreTake(doneSem, portMAX_DELAY);
  finalize();
}

void start() {
  if (!doneSem) {
    doneSem = xSemaphoreCreateBinary();
    if (!doneSem) {
      LOG_ERR("LIBPW", "Cannot create background Library build semaphore");
      gaveUp = true;
      return;
    }
  }
  startGeneration = Storage.libraryContentGeneration();
  useMetadata = SETTINGS.libraryUseMetadata != 0;
  buildOk = false;
  buildCancelled = false;
  startedAtMs = millis();
  pausedMs = 0;
  paused.store(false, std::memory_order_release);
  cancelRequested.store(false, std::memory_order_release);
  // Dual-core: priority 1 on core 0, which the loop and render tasks do not
  // use. At idle priority it would time-slice with IDLE0 (which does not yield)
  // and get about half the core. Single-core: idle priority, so the walk only
  // gets the CPU when the loop and render tasks have nothing to do. Mutex
  // priority inheritance covers the card lock either way.
  const bool dualCore = portNUM_PROCESSORS > 1;
  const BaseType_t core = dualCore ? 0 : tskNO_AFFINITY;
  const UBaseType_t priority = dualCore ? tskIDLE_PRIORITY + 1 : tskIDLE_PRIORITY;
  if (xTaskCreatePinnedToCore(&buildTask, "LibPrewarm", kStackBytes, nullptr, priority, &task, core) != pdPASS) {
    task = nullptr;
    gaveUp = true;
    LOG_ERR("LIBPW", "Cannot start background Library build (%u free, %u max alloc)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return;
  }
  LOG_INF("LIBPW", "Background Library build started (%u free, %u max alloc)", ESP.getFreeHeap(),
          ESP.getMaxAllocHeap());
}
}  // namespace

void tick(const bool idle) {
  collectIfDone();
  if (task) {
    if (idle && paused.load(std::memory_order_relaxed)) {
      paused.store(false, std::memory_order_release);
      wake();
    } else if (!idle) {
      paused.store(true, std::memory_order_release);
    }
    return;
  }
  if (!idle || gaveUp || Storage.libraryScanCurrent()) return;
  if (ESP.getFreeHeap() < kMinFreeHeap || ESP.getMaxAllocHeap() < kMinMaxAlloc) {
    if (lastSkip != SkipReason::LowHeap) {
      lastSkip = SkipReason::LowHeap;
      LOG_INF("LIBPW", "Background Library build waiting for heap (%u free, %u max alloc)", ESP.getFreeHeap(),
              ESP.getMaxAllocHeap());
    }
    return;
  }
  lastSkip = SkipReason::None;
  start();
}

void pause() {
  // Only an atomic store, so the render task may call it too. start() clears it.
  paused.store(true, std::memory_order_release);
}

void stop(const bool handOffToLibrary) {
  collectIfDone();
  if (!task) return;
  if (handOffToLibrary) {
    paused.store(false, std::memory_order_release);
    wake();
    return;
  }
  cancelRequested.store(true, std::memory_order_release);
  wake();
  waitForDone();
}

bool active() { return task != nullptr; }

bool working() { return task != nullptr && !paused.load(std::memory_order_relaxed); }

bool finishForLibrary(const uint32_t waitMs) {
  collectIfDone();
  if (!task) return true;
  paused.store(false, std::memory_order_release);
  wake();
  if (waitMs != UINT32_MAX) {
    if (xSemaphoreTake(doneSem, pdMS_TO_TICKS(waitMs)) != pdTRUE) return false;
    finalize();
    return true;
  }
  waitForDone();
  return true;
}

#endif

}  // namespace LibraryPrewarm
