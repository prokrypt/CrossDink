#include "UsbDriveReadAhead.h"

#if FREEINK_CAP_USB_MSC

#include <Logging.h>
#include <TaskCores.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

namespace {
// Holding the device mutex across one 4 KB SD read is ~2 ms, so a reader that
// catches the prefetch mid-chunk waits at most a few ticks before falling back.
constexpr int kPendingWaitTicks = 5;
}  // namespace

bool UsbDriveReadAhead::begin(FsBlockDeviceInterface* innerDevice) {
  inner = innerDevice;
  generation = 0;
  windowBase = 0;
  windowCount = 0;
  windowHead = 0;
  stopRequested = false;
  prefetchEnabled = false;
  if (!deviceMutex) deviceMutex = xSemaphoreCreateMutex();
  if (!windowMutex) windowMutex = xSemaphoreCreateMutex();
  if (!stopDone) stopDone = xSemaphoreCreateBinary();
  if (!deviceMutex || !windowMutex || !stopDone) {
    LOG_ERR("USB", "USB Drive read-ahead mutex allocation failed; reading directly");
    return false;
  }

  // The window is only ever memcpy'd, never handed to DMA, so PSRAM is fine and
  // keeps 64 KB out of internal RAM.
  window = static_cast<uint8_t*>(heap_caps_malloc(kWindowSectors * kSectorSize, MALLOC_CAP_SPIRAM));
  if (!window) window = static_cast<uint8_t*>(heap_caps_malloc(kWindowSectors * kSectorSize, MALLOC_CAP_8BIT));
  staging = static_cast<uint8_t*>(heap_caps_malloc(kChunkSectors * kSectorSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!window || !staging) {
    LOG_ERR("USB", "USB Drive read-ahead buffer allocation failed; reading directly");
    end();
    return false;
  }
  // Below TinyUSB's device task so USB servicing always wins the CPU. usbd is
  // unpinned (esp32-hal-tinyusb.c); pin the read-ahead to the render core,
  // which is idle during USB Drive, so SD prefetch never competes with the
  // loop on core 0 and usbd can take whichever core is free.
  if (xTaskCreatePinnedToCore(prefetchTask, "usbReadAhead", 3072, this, configMAX_PRIORITIES - 2, &task,
                              TaskCores::kUi) != pdPASS) {
    task = nullptr;
    LOG_ERR("USB", "USB Drive read-ahead task creation failed; reading directly");
    end();
    return false;
  }
  prefetchEnabled = true;
  return true;
}

void UsbDriveReadAhead::end() {
  if (task) {
    stopRequested = true;
    xTaskNotifyGive(task);
    if (xSemaphoreTake(stopDone, pdMS_TO_TICKS(1000)) != pdTRUE) {
      LOG_ERR("USB", "USB Drive read-ahead task did not stop; deleting it");
      lockDevice();
      vTaskDelete(task);
      unlockDevice();
    }
    task = nullptr;
  }
  prefetchEnabled = false;
  heap_caps_free(window);
  window = nullptr;
  heap_caps_free(staging);
  staging = nullptr;
}

void UsbDriveReadAhead::prefetchTask(void* arg) {
  auto* const self = static_cast<UsbDriveReadAhead*>(arg);
  self->prefetchLoop();
  xSemaphoreGive(self->stopDone);
  vTaskDelete(nullptr);
}

void UsbDriveReadAhead::prefetchLoop() {
  const Sector_t total = inner->sectorCount();
  while (!stopRequested) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (!stopRequested) {
      xSemaphoreTake(windowMutex, portMAX_DELAY);
      const Sector_t next = windowBase + windowCount;
      const size_t room = kWindowSectors - windowCount;
      const uint32_t gen = generation;
      xSemaphoreGive(windowMutex);
      if (room < kChunkSectors || next >= total) break;
      const size_t count = std::min<size_t>(kChunkSectors, total - next);

      xSemaphoreTake(deviceMutex, portMAX_DELAY);
      const bool ok = inner->readSectors(next, staging, count);
      xSemaphoreGive(deviceMutex);
      if (!ok) break;  // leave the error for the host's own read to report

      xSemaphoreTake(windowMutex, portMAX_DELAY);
      if (gen == generation && next == windowBase + windowCount) {
        const size_t tail = (windowHead + windowCount) % kWindowSectors;
        const size_t first = std::min(count, kWindowSectors - tail);
        memcpy(window + tail * kSectorSize, staging, first * kSectorSize);
        memcpy(window, staging + first * kSectorSize, (count - first) * kSectorSize);
        windowCount += count;
      }
      xSemaphoreGive(windowMutex);
    }
  }
}

void UsbDriveReadAhead::copyFromWindow(const Sector_t sector, uint8_t* dst, const size_t ns) const {
  const size_t start = (windowHead + (sector - windowBase)) % kWindowSectors;
  const size_t first = std::min(ns, kWindowSectors - start);
  memcpy(dst, window + start * kSectorSize, first * kSectorSize);
  memcpy(dst + first * kSectorSize, window, (ns - first) * kSectorSize);
}

void UsbDriveReadAhead::resetWindow(const Sector_t nextSector) {
  generation++;
  windowBase = nextSector;
  windowCount = 0;
  windowHead = 0;
}

bool UsbDriveReadAhead::readSectors(const Sector_t sector, uint8_t* dst, const size_t ns) {
  if (prefetchEnabled && ns <= kWindowSectors) {
    for (int attempt = 0;; attempt++) {
      xSemaphoreTake(windowMutex, portMAX_DELAY);
      const Sector_t frontier = windowBase + windowCount;
      if (sector >= windowBase && sector + ns <= frontier) {
        copyFromWindow(sector, dst, ns);
        // The host reads forward, so everything up to this request is spent;
        // freeing it lets the prefetch task keep the window full.
        const size_t consumed = sector + ns - windowBase;
        windowBase += consumed;
        windowHead = (windowHead + consumed) % kWindowSectors;
        windowCount -= consumed;
        xSemaphoreGive(windowMutex);
        xTaskNotifyGive(task);
        return true;
      }
      // The request starts inside the window or right at its frontier: the
      // chunk it needs is probably being read now, so wait for it.
      const bool pending = sector >= windowBase && sector <= frontier && sector + ns <= windowBase + kWindowSectors;
      xSemaphoreGive(windowMutex);
      if (!pending || attempt >= kPendingWaitTicks) break;
      vTaskDelay(1);
    }
  }

  lockDevice();
  const bool ok = inner->readSectors(sector, dst, ns);
  unlockDevice();
  if (prefetchEnabled) {
    xSemaphoreTake(windowMutex, portMAX_DELAY);
    resetWindow(sector + ns);
    xSemaphoreGive(windowMutex);
    xTaskNotifyGive(task);
  }
  return ok;
}

bool UsbDriveReadAhead::writeSectors(const Sector_t sector, const uint8_t* src, const size_t ns) {
  lockDevice();
  const bool ok = inner->writeSectors(sector, src, ns);
  unlockDevice();
  if (prefetchEnabled) {
    // Any read-ahead may now be stale. Start over after the write; the next
    // read re-arms prefetching from wherever the host goes.
    xSemaphoreTake(windowMutex, portMAX_DELAY);
    resetWindow(sector + ns);
    xSemaphoreGive(windowMutex);
  }
  return ok;
}

bool UsbDriveReadAhead::isBusy() {
  lockDevice();
  const bool busy = inner->isBusy();
  unlockDevice();
  return busy;
}

Sector_t UsbDriveReadAhead::sectorCount() { return inner->sectorCount(); }

bool UsbDriveReadAhead::syncDevice() {
  lockDevice();
  const bool ok = inner->syncDevice();
  unlockDevice();
  return ok;
}

#endif
