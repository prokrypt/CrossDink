#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

/**
 * Growable byte buffer for one raw OPDS feed response.
 *
 * Grows by allocate-copy-free in the chosen pool (PSRAM on S3) because the
 * response length is unknown while streaming. Refuses to grow past maxBytes so
 * one oversized feed cannot exhaust PSRAM.
 */
class OpdsPageBuffer {
 public:
  OpdsPageBuffer() = default;
  OpdsPageBuffer(MemoryPool pool, size_t maxBytes) : pool(pool), maxBytes(maxBytes) {}
  OpdsPageBuffer(OpdsPageBuffer&& other) noexcept { *this = std::move(other); }
  OpdsPageBuffer& operator=(OpdsPageBuffer&& other) noexcept {
    bytes = std::move(other.bytes);
    length = std::exchange(other.length, 0);
    capacity = std::exchange(other.capacity, 0);
    pool = other.pool;
    maxBytes = other.maxBytes;
    overflowed = std::exchange(other.overflowed, false);
    return *this;
  }
  OpdsPageBuffer(const OpdsPageBuffer&) = delete;
  OpdsPageBuffer& operator=(const OpdsPageBuffer&) = delete;

  // False once the buffer overflowed maxBytes or an allocation failed; the
  // buffer is then released and stays failed until reset().
  bool append(const uint8_t* data, size_t len);
  void reset();

  const uint8_t* data() const { return bytes.get(); }
  size_t size() const { return length; }
  bool failed() const { return overflowed; }
  bool empty() const { return length == 0; }

 private:
  HeapByteBuffer bytes;
  size_t length = 0;
  size_t capacity = 0;
  MemoryPool pool = MemoryPool::Psram;
  size_t maxBytes = 0;
  bool overflowed = false;
};

/**
 * Small LRU cache of raw OPDS feed responses keyed by absolute URL.
 *
 * Holds bytes rather than parsed entries so the parser keeps its fixed entry
 * array and re-parsing a cached page (tens of KB from PSRAM) is cheap. Only
 * the main loop task touches it; the prefetch task hands pages over through
 * OpdsPagePrefetcher.
 */
class OpdsPageCache {
 public:
  static constexpr size_t MAX_PAGES = 64;

  explicit OpdsPageCache(size_t byteBudget) : byteBudget(byteBudget) {}

  // Returns the cached page and marks it most recently used, or nullptr.
  const OpdsPageBuffer* find(const std::string& url);
  // Takes ownership; evicts least recently used pages to fit the budget.
  // Returns false (and drops the page) when it alone exceeds the budget, or,
  // with mayEvict false (background preloads), when it does not fit as is.
  bool store(const std::string& url, OpdsPageBuffer&& page, bool mayEvict = true);
  bool contains(const std::string& url) const;
  void erase(const std::string& url);
  void clear();

  size_t pageCount() const;
  size_t bytesUsed() const { return usedBytes; }

 private:
  struct Slot {
    PsramString url;  // PSRAM: 64 keys otherwise pin internal RAM
    OpdsPageBuffer page;
    uint32_t lastUse = 0;
    bool used = false;
  };

  Slot* findSlot(const std::string& url);
  const Slot* findSlot(const std::string& url) const;
  void evict(Slot& slot);
  Slot* evictLeastRecentlyUsed();

  Slot slots[MAX_PAGES];
  size_t byteBudget;
  size_t usedBytes = 0;
  uint32_t useClock = 0;
};
