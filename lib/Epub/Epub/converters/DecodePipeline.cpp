#include "DecodePipeline.h"

#include <Arduino.h>
#include <Logging.h>
#include <PerfLog.h>

#include "ImageToFramebufferDecoder.h"
#include "TaskCores.h"

namespace {
// JPEGDEC/PNGdec keep their state in their heap objects; the worker's stack
// only carries the decode loop, SD reads and the draw callback's copy.
constexpr uint32_t WORKER_STACK_BYTES = 8192;
}  // namespace

DecodePipeline::~DecodePipeline() {
  if (freeSlots_) vQueueDelete(freeSlots_);
  if (filledSlots_) vQueueDelete(filledSlots_);
  if (workerDone_) vSemaphoreDelete(workerDone_);
}

bool DecodePipeline::worthSplitting() {
#if defined(portNUM_PROCESSORS) && portNUM_PROCESSORS > 1
  return psramHeapAvailable() && xPortGetCoreID() != TaskCores::kWorker;
#else
  return false;
#endif
}

bool DecodePipeline::begin(const size_t slotBytes) {
  // Three slots: one being decoded, one being drawn, one spare so neither
  // side waits on a single hand-off.
  ring_ = makePsramByteBufferNoThrow(slotBytes * SLOT_COUNT);
  freeSlots_ = xQueueCreate(SLOT_COUNT, sizeof(int));
  // +1 so the end marker always fits behind every filled slot.
  filledSlots_ = xQueueCreate(SLOT_COUNT + 1, sizeof(int));
  workerDone_ = xSemaphoreCreateBinary();
  if (!ring_ || !freeSlots_ || !filledSlots_ || !workerDone_) {
    LOG_ERR("IMG", "Decode pipeline unavailable (%u B ring); decoding inline",
            static_cast<unsigned>(slotBytes * SLOT_COUNT));
    return false;
  }
  slotBytes_ = slotBytes;
  for (int i = 0; i < SLOT_COUNT; i++) xQueueSend(freeSlots_, &i, 0);
  return true;
}

uint8_t* DecodePipeline::acquire() {
  // The decoder can run for seconds; let IDLE0 feed the task watchdog.
  ImageToFramebufferDecoder::yieldDuringDecode(workerLastYieldMs_);
  if (stopped_.load(std::memory_order_relaxed)) return nullptr;
  int slot = -1;
  xQueueReceive(freeSlots_, &slot, portMAX_DELAY);
  workerSlot_ = slot;
  return ring_.get() + static_cast<size_t>(slot) * slotBytes_;
}

void DecodePipeline::commit(const Block& block) {
  const int slot = workerSlot_;
  workerSlot_ = -1;
  slots_[slot] = block;
  slots_[slot].pixels = ring_.get() + static_cast<size_t>(slot) * slotBytes_;
  xQueueSend(filledSlots_, &slot, portMAX_DELAY);
}

void DecodePipeline::workerMain(void* param) {
  auto* self = static_cast<DecodePipeline*>(param);
  self->result_ = self->decode_(self->decodeContext_);
  const int end = END_OF_DECODE;
  xQueueSend(self->filledSlots_, &end, portMAX_DELAY);
  PerfLog::noteTaskExit("ImgDecode");
  // The pipeline may be destroyed as soon as this is given.
  xSemaphoreGive(self->workerDone_);
  vTaskDelete(nullptr);
}

bool DecodePipeline::run(const DecodeFn decode, void* decodeContext, const ConsumeFn consume, void* consumeContext,
                         int& rc) {
  decode_ = decode;
  decodeContext_ = decodeContext;
  stopped_.store(false, std::memory_order_relaxed);
  workerLastYieldMs_ = millis();
  if (xTaskCreatePinnedToCore(workerMain, "ImgDecode", WORKER_STACK_BYTES, this, 1, nullptr, TaskCores::kWorker) !=
      pdPASS) {
    LOG_ERR("IMG", "Cannot start decode worker; decoding inline");
    return false;
  }
  const uint32_t startedAt = millis();
  uint32_t blocks = 0;
  for (;;) {
    int slot = END_OF_DECODE;
    xQueueReceive(filledSlots_, &slot, portMAX_DELAY);
    if (slot == END_OF_DECODE) break;
    // After a stop, keep returning slots so a decoder blocked in acquire()
    // wakes, sees the stop and ends.
    if (!stopped_.load(std::memory_order_relaxed) && !consume(consumeContext, slots_[slot])) {
      stopped_.store(true, std::memory_order_relaxed);
    }
    blocks++;
    xQueueSend(freeSlots_, &slot, 0);
  }
  xSemaphoreTake(workerDone_, portMAX_DELAY);
  rc = result_;
  LOG_DBG("IMG", "Split decode: %u blocks in %ums", static_cast<unsigned>(blocks),
          static_cast<unsigned>(millis() - startedAt));
  return true;
}
