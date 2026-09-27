#pragma once

#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

// Splits an image decode across the cores: the decoder runs on the worker core
// and hands each decoded block to the calling task through a small ring of
// PSRAM slots, while the caller dithers and draws blocks in decode order. The
// caller keeps sole use of the renderer and its framebuffer.
class DecodePipeline {
 public:
  // A decoded block. Its meaning beyond `pixels` is the decoder's own; the
  // pipeline only moves it.
  struct Block {
    const uint8_t* pixels = nullptr;
    int x = 0;
    int y = 0;
    int width = 0;
    int widthUsed = 0;
    int height = 0;
  };
  using DecodeFn = int (*)(void* context);                        // worker core; returns the decoder's rc
  using ConsumeFn = bool (*)(void* context, const Block& block);  // caller; false stops the decode

  DecodePipeline() = default;
  ~DecodePipeline();
  DecodePipeline(const DecodePipeline&) = delete;
  DecodePipeline& operator=(const DecodePipeline&) = delete;

  // True when a split can help: a second core, PSRAM, and a caller that is not
  // itself on the worker core.
  static bool worthSplitting();

  // Allocates the slot ring. False (logged) when the decode should stay inline.
  bool begin(size_t slotBytes);
  size_t slotBytes() const { return slotBytes_; }

  // Worker side, from the decoder's draw callback: waits for a free slot and
  // returns it, or nullptr once the consumer has stopped the decode.
  uint8_t* acquire();
  // Hands the slot from acquire() to the caller. block.pixels is ignored.
  void commit(const Block& block);

  // Caller side: runs decode on the worker core and consume here until the
  // decode ends. False when the worker could not start (decode did not run);
  // otherwise rc is decode's result.
  bool run(DecodeFn decode, void* decodeContext, ConsumeFn consume, void* consumeContext, int& rc);

 private:
  static constexpr int SLOT_COUNT = 3;
  static constexpr int END_OF_DECODE = -1;
  static void workerMain(void* param);

  HeapByteBuffer ring_;  // SLOT_COUNT * slotBytes_, PSRAM
  size_t slotBytes_ = 0;
  Block slots_[SLOT_COUNT]{};
  int workerSlot_ = -1;             // worker only
  uint32_t workerLastYieldMs_ = 0;  // worker only
  QueueHandle_t freeSlots_ = nullptr;
  QueueHandle_t filledSlots_ = nullptr;
  SemaphoreHandle_t workerDone_ = nullptr;
  std::atomic<bool> stopped_{false};
  DecodeFn decode_ = nullptr;
  void* decodeContext_ = nullptr;
  int result_ = 0;
};
