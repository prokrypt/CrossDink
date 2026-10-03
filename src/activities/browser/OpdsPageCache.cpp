#include "OpdsPageCache.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

namespace {
constexpr size_t INITIAL_PAGE_CAPACITY = 16 * 1024;
constexpr std::string_view UPDATED_OPEN = "<updated>";
constexpr std::string_view UPDATED_CLOSE = "</updated>";

// The bytes from pos up to the next <updated> element; moves pos past that
// element (or to the end).
std::string_view nextRunOutsideUpdated(const std::string_view body, size_t& pos) {
  const size_t start = pos;
  const size_t open = body.find(UPDATED_OPEN, start);
  if (open == std::string_view::npos) {
    pos = body.size();
    return body.substr(start);
  }
  const size_t close = body.find(UPDATED_CLOSE, open);
  pos = close == std::string_view::npos ? body.size() : close + UPDATED_CLOSE.size();
  return body.substr(start, open - start);
}

std::string_view bodyOf(const OpdsPageBuffer& page) {
  return {reinterpret_cast<const char*>(page.data()), page.size()};
}
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
                                   [&url](const Slot& slot) { return slot.used && std::string_view(slot.url) == url; });
  return found == std::end(slots) ? nullptr : found;
}

const OpdsPageCache::Slot* OpdsPageCache::findSlot(const std::string& url) const {
  const Slot* const found = std::find_if(std::begin(slots), std::end(slots), [&url](const Slot& slot) {
    return slot.used && std::string_view(slot.url) == url;
  });
  return found == std::end(slots) ? nullptr : found;
}

const OpdsPageBuffer* OpdsPageCache::find(const std::string& url) {
  Slot* slot = findSlot(url);
  if (!slot) return nullptr;
  slot->lastUse = ++useClock;
  return &slot->page;
}

bool OpdsPageCache::contains(const std::string& url) const { return findSlot(url) != nullptr; }

bool OpdsPageCache::sameFeed(const OpdsPageBuffer& a, const OpdsPageBuffer& b) {
  const std::string_view x = bodyOf(a);
  const std::string_view y = bodyOf(b);
  size_t i = 0;
  size_t j = 0;
  while (i < x.size() || j < y.size()) {
    if (nextRunOutsideUpdated(x, i) != nextRunOutsideUpdated(y, j)) return false;
  }
  return true;
}

void OpdsPageCache::erase(const std::string& url) {
  if (Slot* slot = findSlot(url)) evict(*slot);
}

void OpdsPageCache::evict(Slot& slot) {
  usedBytes -= slot.page.size();
  slot.page.reset();
  slot.url.clear();
  slot.url.shrink_to_fit();
  slot.used = false;
  ++changeCount;
}

OpdsPageCache::Slot* OpdsPageCache::evictLeastRecentlyUsed() {
  Slot* oldest = nullptr;
  for (auto& slot : slots) {
    if (slot.used && (!oldest || slot.lastUse < oldest->lastUse)) oldest = &slot;
  }
  if (oldest) evict(*oldest);
  return oldest;
}

bool OpdsPageCache::store(const std::string& url, OpdsPageBuffer&& page, const bool mayEvict,
                          const uint32_t fetchedMs) {
  if (page.empty() || page.failed() || page.size() > byteBudget) return false;
  if (!mayEvict && (usedBytes + page.size() > byteBudget || pageCount() >= MAX_PAGES) && !findSlot(url)) {
    return false;
  }

  if (Slot* existing = findSlot(url)) evict(*existing);
  while (usedBytes + page.size() > byteBudget && evictLeastRecentlyUsed()) {
  }

  Slot* target = std::find_if(std::begin(slots), std::end(slots), [](const Slot& slot) { return !slot.used; });
  if (target == std::end(slots)) target = evictLeastRecentlyUsed();

  target->url.assign(url.data(), url.size());
  ++changeCount;
  target->page = std::move(page);
  target->lastUse = ++useClock;
  target->fetchedMs = fetchedMs;
  target->used = true;
  usedBytes += target->page.size();
  return true;
}

void OpdsPageCache::markFetched(const std::string& url, const uint32_t nowMs) {
  if (Slot* slot = findSlot(url)) slot->fetchedMs = nowMs;
}

bool OpdsPageCache::fetchedWithin(const std::string& url, const uint32_t nowMs, const uint32_t windowMs) const {
  const Slot* slot = findSlot(url);
  return slot && slot->fetchedMs != 0 && nowMs - slot->fetchedMs < windowMs;
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

namespace {
std::unique_ptr<OpdsPageCache> sHandoff;
}  // namespace

namespace opds_page_cache_handoff {
void give(std::unique_ptr<OpdsPageCache> cache) { sHandoff = std::move(cache); }
std::unique_ptr<OpdsPageCache> take() { return std::move(sHandoff); }
}  // namespace opds_page_cache_handoff
