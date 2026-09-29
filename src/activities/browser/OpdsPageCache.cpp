#include "OpdsPageCache.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <utility>

namespace {
constexpr size_t INITIAL_PAGE_CAPACITY = 16 * 1024;
}  // namespace

bool OpdsPageBuffer::append(const uint8_t* data, const size_t len) {
  if (overflowed) return false;
  if (len == 0) return true;
  if (len > maxBytes - length) {
    reset();
    overflowed = true;
    return false;
  }

  const size_t needed = length + len;
  if (needed > capacity) {
    size_t newCapacity = capacity == 0 ? INITIAL_PAGE_CAPACITY : capacity;
    while (newCapacity < needed) newCapacity *= 2;
    if (newCapacity > maxBytes) newCapacity = maxBytes;

    HeapByteBuffer grown = makeAlignedByteBufferNoThrow(newCapacity, pool);
    if (!grown) {
      reset();
      overflowed = true;
      return false;
    }
    if (length > 0) memcpy(grown.get(), bytes.get(), length);
    bytes = std::move(grown);
    capacity = newCapacity;
  }

  memcpy(bytes.get() + length, data, len);
  length = needed;
  return true;
}

void OpdsPageBuffer::reset() {
  bytes.reset();
  length = 0;
  capacity = 0;
  overflowed = false;
}

OpdsPageCache::Slot* OpdsPageCache::findSlot(const std::string& url) {
  Slot* const found = std::find_if(std::begin(slots), std::end(slots),
                                   [&url](const Slot& slot) { return slot.used && slot.url == url; });
  return found == std::end(slots) ? nullptr : found;
}

const OpdsPageCache::Slot* OpdsPageCache::findSlot(const std::string& url) const {
  const Slot* const found = std::find_if(std::begin(slots), std::end(slots),
                                         [&url](const Slot& slot) { return slot.used && slot.url == url; });
  return found == std::end(slots) ? nullptr : found;
}

const OpdsPageBuffer* OpdsPageCache::find(const std::string& url) {
  Slot* slot = findSlot(url);
  if (!slot) return nullptr;
  slot->lastUse = ++useClock;
  return &slot->page;
}

bool OpdsPageCache::contains(const std::string& url) const { return findSlot(url) != nullptr; }

void OpdsPageCache::erase(const std::string& url) {
  if (Slot* slot = findSlot(url)) evict(*slot);
}

void OpdsPageCache::evict(Slot& slot) {
  usedBytes -= slot.page.size();
  slot.page.reset();
  slot.url.clear();
  slot.url.shrink_to_fit();
  slot.used = false;
}

OpdsPageCache::Slot* OpdsPageCache::evictLeastRecentlyUsed() {
  Slot* oldest = nullptr;
  for (auto& slot : slots) {
    if (slot.used && (!oldest || slot.lastUse < oldest->lastUse)) oldest = &slot;
  }
  if (oldest) evict(*oldest);
  return oldest;
}

bool OpdsPageCache::store(const std::string& url, OpdsPageBuffer&& page, const bool mayEvict) {
  if (page.empty() || page.failed() || page.size() > byteBudget) return false;
  if (!mayEvict && (usedBytes + page.size() > byteBudget || pageCount() >= MAX_PAGES) && !findSlot(url)) {
    return false;
  }

  if (Slot* existing = findSlot(url)) evict(*existing);
  while (usedBytes + page.size() > byteBudget && evictLeastRecentlyUsed()) {
  }

  Slot* target = std::find_if(std::begin(slots), std::end(slots), [](const Slot& slot) { return !slot.used; });
  if (target == std::end(slots)) target = evictLeastRecentlyUsed();

  target->url = url;
  target->page = std::move(page);
  target->lastUse = ++useClock;
  target->used = true;
  usedBytes += target->page.size();
  return true;
}

void OpdsPageCache::clear() {
  for (auto& slot : slots) {
    if (slot.used) evict(slot);
  }
  useClock = 0;
}

size_t OpdsPageCache::pageCount() const {
  return static_cast<size_t>(
      std::count_if(std::begin(slots), std::end(slots), [](const Slot& slot) { return slot.used; }));
}
