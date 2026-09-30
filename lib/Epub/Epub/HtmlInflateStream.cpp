#include "HtmlInflateStream.h"

#include <Arduino.h>
#include <BuildScratch.h>
#include <Logging.h>
#include <PerfLog.h>
#include <MemoryBudget.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

#include "Epub.h"
#include "TaskCores.h"

namespace {
// The inflater keeps its window and buffers on the heap; the task stack holds
// the ZIP entry lookup, SD reads and the tee writes.
constexpr uint32_t WORKER_STACK_BYTES = 8192;
}  // namespace

// Print sink for Epub::readItemContentsToStream(): appends to the PSRAM buffer
// and the cache file. A short write stops the inflate (stop request, overflow
// or SD write failure).
class HtmlInflateStream::Sink final : public Print {
 public:
  explicit Sink(HtmlInflateStream& owner) : owner_(owner) {}

  size_t write(uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* data, size_t size) override {
    if (owner_.stopRequested_.load(std::memory_order_relaxed)) return 0;
    const size_t at = owner_.written_.load(std::memory_order_relaxed);
    if (size > owner_.capacity_ - at) {
      LOG_ERR("SCT", "Inflated HTML exceeds its entry size (%u)", static_cast<unsigned>(owner_.capacity_));
      return 0;
    }
    if (owner_.cacheFile_.write(data, size) != size) {
      LOG_ERR("SCT", "HTML cache write failed during overlapped inflate");
      return 0;
    }
    memcpy(owner_.buffer_.get() + at, data, size);
    owner_.publish(at + size);
    return size;
  }

 private:
  HtmlInflateStream& owner_;
};

HtmlInflateStream::~HtmlInflateStream() {
  finish(/*stopEarly=*/true);
  if (dataReady_) vSemaphoreDelete(dataReady_);
  if (workerDone_) vSemaphoreDelete(workerDone_);
}

bool HtmlInflateStream::worthSplitting() {
#if defined(portNUM_PROCESSORS) && portNUM_PROCESSORS > 1
  return psramHeapAvailable() && xPortGetCoreID() != TaskCores::kWorker;
#else
  return false;
#endif
}

bool HtmlInflateStream::start(const Epub& epub, const std::string& itemHref, const size_t itemBytes,
                              const size_t chunkSize, HalFile& cacheFile) {
  if (itemBytes == 0 || itemBytes > MAX_ITEM_BYTES || !MemoryBudget::canAllocatePsram(itemBytes)) return false;
  buffer_ = makePsramByteBufferNoThrow(itemBytes);
  if (!dataReady_) dataReady_ = xSemaphoreCreateBinary();
  if (!workerDone_) workerDone_ = xSemaphoreCreateBinary();
  if (!buffer_ || !dataReady_ || !workerDone_) {
    LOG_ERR("SCT", "Overlapped inflate unavailable (%u B); inflating first", static_cast<unsigned>(itemBytes));
    buffer_.reset();
    return false;
  }
  epub_ = &epub;
  scratchLender_ = buildscratch::self();
  itemHref_ = itemHref;
  chunkSize_ = chunkSize;
  capacity_ = itemBytes;
  written_.store(0, std::memory_order_relaxed);
  readPos_ = 0;
  stopRequested_.store(false, std::memory_order_relaxed);
  cacheFile_ = std::move(cacheFile);
  state_.store(State::Running, std::memory_order_release);
  joined_ = false;
  if (xTaskCreatePinnedToCore(workerMain, "HtmlInflate", WORKER_STACK_BYTES, this, 1, nullptr, TaskCores::kWorker) !=
      pdPASS) {
    LOG_ERR("SCT", "Cannot start inflate worker; inflating first");
    cacheFile = std::move(cacheFile_);
    state_.store(State::Idle, std::memory_order_relaxed);
    joined_ = true;
    buffer_.reset();
    return false;
  }
  return true;
}

void HtmlInflateStream::publish(const size_t bytes) {
  written_.store(bytes, std::memory_order_release);
  xSemaphoreGive(dataReady_);
}

void HtmlInflateStream::workerMain(void* param) {
  auto* self = static_cast<HtmlInflateStream*>(param);
  const uint32_t startedAt = millis();
  Sink sink(*self);
  bool ok;
  {
    // The starter joins this worker before it ends its framebuffer loan, so
    // the inflater may use the lent bytes like an inline build would.
    buildscratch::HelperScope scratch(self->scratchLender_);
    ok = self->epub_->readItemContentsToStream(self->itemHref_, sink, self->chunkSize_);
  }
  const size_t bytes = self->written_.load(std::memory_order_relaxed);
  if (ok && bytes != self->capacity_) {
    LOG_ERR("SCT", "Inflated %u B of a %u B entry", static_cast<unsigned>(bytes),
            static_cast<unsigned>(self->capacity_));
    ok = false;
  }
  self->cacheFile_.close();
  LOG_DBG("SCT", "Overlapped inflate on core %d: ok=%u %u B in %lums", xPortGetCoreID(), ok ? 1U : 0U,
          static_cast<unsigned>(bytes), millis() - startedAt);
  self->state_.store(ok ? State::Done : State::Failed, std::memory_order_release);
  xSemaphoreGive(self->dataReady_);
  PerfLog::noteTaskExit("HtmlInflate");
  // The stream may be destroyed as soon as this is given.
  xSemaphoreGive(self->workerDone_);
  vTaskDelete(nullptr);
}

size_t HtmlInflateStream::read(void* dst, const size_t maxBytes) {
  for (;;) {
    // Read the state before the byte count: bytes published before Done are
    // then always seen.
    const State state = state_.load(std::memory_order_acquire);
    const size_t available = written_.load(std::memory_order_acquire);
    if (available > readPos_) {
      const size_t n = std::min(maxBytes, available - readPos_);
      memcpy(dst, buffer_.get() + readPos_, n);
      readPos_ += n;
      return n;
    }
    if (state != State::Running) return 0;
    xSemaphoreTake(dataReady_, portMAX_DELAY);
  }
}

bool HtmlInflateStream::drained() const {
  return state_.load(std::memory_order_acquire) == State::Done && readPos_ == written_.load(std::memory_order_acquire);
}

bool HtmlInflateStream::finish(const bool stopEarly) {
  if (!joined_) {
    if (stopEarly) stopRequested_.store(true, std::memory_order_relaxed);
    xSemaphoreTake(workerDone_, portMAX_DELAY);
    joined_ = true;
  }
  buffer_.reset();
  return state_.load(std::memory_order_acquire) == State::Done;
}
