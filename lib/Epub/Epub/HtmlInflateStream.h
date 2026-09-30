#pragma once

#include <HalStorage.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class Epub;

// Overlaps a chapter's ZIP inflate with its parse on a section's first build.
// A worker-core task inflates the spine item into a PSRAM buffer sized to the
// whole item (so it never waits on the parser) and tees it into the HTML
// cache file; the parser reads the buffer as it fills. Without it the parser
// waits for the full inflate to reach the SD card and reads it back.
class HtmlInflateStream {
 public:
  // Largest item overlapped; bigger chapters inflate to the SD card first.
  static constexpr size_t MAX_ITEM_BYTES = 4U * 1024U * 1024U;

  HtmlInflateStream() = default;
  ~HtmlInflateStream();
  HtmlInflateStream(const HtmlInflateStream&) = delete;
  HtmlInflateStream& operator=(const HtmlInflateStream&) = delete;

  // A second core is free to inflate (the caller is not on it) and PSRAM is
  // present.
  static bool worthSplitting();

  // Starts inflating itemHref into memory and into cacheFile (opened for
  // write; closed by the worker when it ends). False, with cacheFile still
  // open, when the overlap cannot run.
  bool start(const Epub& epub, const std::string& itemHref, size_t itemBytes, size_t chunkSize, HalFile& cacheFile);

  // Parser side. Copies up to maxBytes of inflated HTML, waiting for the
  // worker when it has none ready. Returns 0 only once the inflate has ended.
  size_t read(void* dst, size_t maxBytes);
  // Everything inflated has been read and the inflate succeeded.
  bool drained() const;
  bool failed() const { return state_.load(std::memory_order_acquire) == State::Failed; }
  size_t totalBytes() const { return capacity_; }

  // Stops the inflate early (a cancelled or failed parse) and waits for the
  // worker; true when it completed successfully. Safe to call repeatedly.
  bool finish(bool stopEarly);

 private:
  enum class State : uint8_t { Idle, Running, Done, Failed };
  class Sink;
  static void workerMain(void* param);
  void publish(size_t bytes);

  const Epub* epub_ = nullptr;
  const void* scratchLender_ = nullptr;  // starter task; may share its build scratch
  std::string itemHref_;
  size_t chunkSize_ = 0;
  HalFile cacheFile_;
  HeapByteBuffer buffer_;  // PSRAM, capacity_ bytes
  size_t capacity_ = 0;
  std::atomic<size_t> written_{0};
  size_t readPos_ = 0;  // parser only
  std::atomic<State> state_{State::Idle};
  std::atomic<bool> stopRequested_{false};
  SemaphoreHandle_t dataReady_ = nullptr;
  SemaphoreHandle_t workerDone_ = nullptr;
  bool joined_ = true;
};
