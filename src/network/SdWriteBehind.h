#pragma once

#include <cstddef>
#include <cstdint>

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR)
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

// Double-buffered SD writer for network transfers. The receiving task copies
// data into one 32 KB PSRAM buffer while a writer task on the other core
// writes the full one, so receive and SD write overlap (X4 Pro logs: the SD
// write took ~22-40% of transfer wall time on the receiving task). The
// buffers and the task exist only between begin() and finish()/abort(); if
// begin() fails the caller writes directly, as before.
class SdWriteBehind {
 public:
  // Called on the writer task; true when all `len` bytes were written.
  using WriteFn = bool (*)(void* ctx, const uint8_t* data, size_t len);

#if defined(BOARD_HAS_PSRAM) && !defined(SIMULATOR)
  SdWriteBehind() = default;
  ~SdWriteBehind() { abort(); }
  SdWriteBehind(const SdWriteBehind&) = delete;
  SdWriteBehind& operator=(const SdWriteBehind&) = delete;

  bool begin(WriteFn fn, void* ctx, const char* taskName, BaseType_t core);
  bool active() const { return task != nullptr; }
  // False once any earlier write came up short.
  bool append(const uint8_t* data, size_t len);
  // Writes what is buffered and stops the task. True when every byte landed.
  bool finish();
  // Stops the task without writing the tail (the transfer is being discarded).
  void abort();

 private:
  static constexpr size_t BUF_BYTES = 32 * 1024;

  bool submit();
  void stopTask();
  void release();
  static void taskMain(void* param);

  WriteFn writeFn = nullptr;
  void* writeCtx = nullptr;
  uint8_t* bufs[2] = {nullptr, nullptr};
  size_t fill = 0;
  size_t fillLen = 0;
  const uint8_t* pendingBuf = nullptr;
  size_t pendingLen = 0;
  volatile bool failed = false;
  volatile bool quit = false;
  SemaphoreHandle_t work = nullptr;
  SemaphoreHandle_t idle = nullptr;
  TaskHandle_t task = nullptr;
#else
  bool begin(WriteFn, void*, const char*, int) { return false; }
  bool active() const { return false; }
  bool append(const uint8_t*, size_t) { return false; }
  bool finish() { return true; }
  void abort() {}
#endif
};
