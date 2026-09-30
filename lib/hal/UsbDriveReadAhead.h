#pragma once

#if FREEINK_CAP_USB_MSC

#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

struct UsbDriveIo;

// Block-device decorator used while USB Drive exposes the SD card. TinyUSB's
// MSC driver serves reads in 4 KB chunks and waits for each SD read before it
// starts the USB transfer, so card latency adds to every chunk. A host mounting
// a FAT32 volume streams the whole FAT sequentially; this keeps the next
// sectors read ahead on a background task while the current chunk is on the
// wire, so sequential reads are served from memory.
class UsbDriveReadAhead : public FsBlockDeviceInterface {
 public:
  // Allocates the read-ahead window and starts the prefetch task. On failure it
  // logs and still forwards every call to `inner` without read-ahead.
  bool begin(FsBlockDeviceInterface* inner);
  // Stops the prefetch task and frees the window. Must run before the inner
  // device is ended or remounted. If the task is stuck in a card read it is
  // left to exit on its own, and the next begin() frees its buffers.
  void end() override;

  bool isBusy() override;
  bool readSector(Sector_t sector, uint8_t* dst) override { return readSectors(sector, dst, 1); }
  bool readSectors(Sector_t sector, uint8_t* dst, size_t ns) override;
  Sector_t sectorCount() override;
  bool syncDevice() override;
  bool writeSector(Sector_t sector, const uint8_t* src) override { return writeSectors(sector, src, 1); }
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t ns) override;
  // Host I/O since begin(): first/last host read/write times, byte totals
  // and call count. Prefetch reads don't count.
  void hostIo(UsbDriveIo& out) const;

 private:
  static constexpr size_t kSectorSize = 512;
  static constexpr size_t kChunkSectors = 8;     // one SDMMC transfer
  static constexpr size_t kWindowSectors = 128;  // 64 KB read-ahead window

  static void prefetchTask(void* arg);
  void noteHostIo();
  void prefetchLoop();
  // Copies window sectors [sector, sector + ns) to dst. Caller holds windowMutex.
  void copyFromWindow(Sector_t sector, uint8_t* dst, size_t ns) const;
  void resetWindow(Sector_t nextSector);
  void freeBuffers();
  void lockDevice() const {
    if (deviceMutex) xSemaphoreTake(deviceMutex, portMAX_DELAY);
  }
  void unlockDevice() const {
    if (deviceMutex) xSemaphoreGive(deviceMutex);
  }

  FsBlockDeviceInterface* inner = nullptr;
  SemaphoreHandle_t deviceMutex = nullptr;  // serializes access to `inner`
  SemaphoreHandle_t windowMutex = nullptr;  // guards the window fields below
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t stopDone = nullptr;  // given by the task as it exits
  volatile bool stopRequested = false;

  uint8_t* window = nullptr;   // ring of kWindowSectors sectors
  uint8_t* staging = nullptr;  // one chunk read by the prefetch task
  Sector_t windowBase = 0;     // first sector held in the window
  size_t windowCount = 0;      // valid sectors starting at windowBase
  size_t windowHead = 0;       // ring index of windowBase
  uint32_t generation = 0;     // bumped on every reset to drop in-flight chunks
  // Set by host reads, cleared by host writes: after a write the sectors ahead
  // are usually the ones the host writes next, so reading them only competes
  // with those writes for the card.
  bool readAheadArmed = false;
  bool prefetchEnabled = false;
  // Written on the TinyUSB task, read on the main loop.
  std::atomic<uint32_t> firstIoMs{0};
  std::atomic<uint32_t> lastIoMs{0};
  std::atomic<uint32_t> hostOps{0};
  std::atomic<uint32_t> hostReadBytes{0};
  std::atomic<uint32_t> hostWriteBytes{0};
};

#endif
