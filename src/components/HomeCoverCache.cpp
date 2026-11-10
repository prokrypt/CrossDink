#include "HomeCoverCache.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include <algorithm>
#include <utility>

#include "RecentBooksStore.h"
#include "UITheme.h"
#include "components/NoCoverPlaceholder.h"

namespace fui = freeink::ui;

void HomeCoverCache::begin() {
  // Cover regions do not overlap. Each may widen by one physical byte per row.
  coverCacheCapacity = renderer.getRegionByteSize(0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()) +
                       MAX_COVERS * std::max(renderer.getScreenWidth(), renderer.getScreenHeight());
  coverCache = makePsramByteBufferNoThrow(coverCacheCapacity);
  if (!coverCache) {
#if defined(SIMULATOR)
    LOG_DBG("HOME", "PSRAM cover cache unavailable in simulator; rendering uncached");
#else
    LOG_ERR("HOME", "PSRAM cover cache unavailable (%u bytes); rendering uncached", unsigned(coverCacheCapacity));
#endif
    coverCacheCapacity = 0;
  }
}

void HomeCoverCache::invalidate() {
  coverCacheUsed = 0;
  cachedCovers.fill(CachedCover{});
}

void HomeCoverCache::invalidate(size_t index) {
  if (index < cachedCovers.size()) cachedCovers[index].valid = false;
}

void HomeCoverCache::prepare() {
  const int orientation = static_cast<int>(renderer.getOrientation());
  if (coverCacheOrientation != orientation || coverCacheInverted != bool(SETTINGS.screenInverted)) {
    invalidate();
    coverCacheOrientation = orientation;
    coverCacheInverted = SETTINGS.screenInverted;
  }
}

bool HomeCoverCache::paint(fui::Rect rect, size_t index, const std::string& path, const RecentBook& book) {
  if (index >= cachedCovers.size()) return false;
  auto& cached = cachedCovers[index];
  if (coverCache && cached.valid && cached.rect.x == rect.x && cached.rect.y == rect.y &&
      cached.rect.width == rect.width && cached.rect.height == rect.height &&
      renderer.copyBufferToRegion(rect.x, rect.y, rect.width, rect.height, coverCache.get() + cached.offset,
                                  cached.bytes)) {
    return true;
  }
  cached.valid = false;
  bool drawn = false;
  if (!path.empty() && Storage.openFileForRead("HOME", path, coverFile)) {
    if (coverBitmap.parseHeaders() == BmpReaderError::Ok && coverBitmap.rewindToData() == BmpReaderError::Ok &&
        coverBitmap.getWidth() > 0 && coverBitmap.getHeight() > 0) {
      const int width = coverBitmap.getWidth();
      const int height = coverBitmap.getHeight();
      const float cropX = width > rect.width ? 1.0f - static_cast<float>(rect.width) / width : 0.0f;
      const float cropY = height > rect.height ? 1.0f - static_cast<float>(rect.height) / height : 0.0f;
      const int x = rect.x + std::max(0, (rect.width - width) / 2);
      const int y = rect.y + std::max(0, (rect.height - height) / 2);
      drawn = renderer.drawBitmap(coverBitmap, x, y, rect.width, rect.height, cropX, cropY);
    }
    coverFile.close();
  }
  if (!drawn) {
    renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
    NoCoverPlaceholder::draw(renderer, rect.x, rect.y, rect.width, rect.height, book);
    renderer.drawRect(rect.x, rect.y, rect.width, rect.height, true);
  }
  if (coverCache) {
    const size_t needed = renderer.getRegionByteSize(rect.x, rect.y, rect.width, rect.height);
    if (needed > cached.bytes && needed <= coverCacheCapacity - coverCacheUsed) {
      cached.offset = coverCacheUsed;
      cached.bytes = needed;
      coverCacheUsed += needed;
    }
    if (needed > 0 && needed <= cached.bytes) {
      cached.rect = rect;
      cached.valid = renderer.copyRegionToBuffer(rect.x, rect.y, rect.width, rect.height,
                                                 coverCache.get() + cached.offset, cached.bytes);
    }
  }
  return true;  // The cover slot is painted, including fallback art.
}

void HomeCoverSlots::begin(const std::vector<RecentBook>& recent) {
  books = &recent;
  if (!recent.empty()) cache.begin();
  refreshPaths();
}

void HomeCoverSlots::refreshPaths() {
  cache.invalidate();
  for (size_t i = 0; i < books->size() && i < paths.size(); ++i) refreshPath(i);
}

void HomeCoverSlots::refreshPath(const size_t index) {
  if (index >= books->size() || index >= paths.size()) return;
  cache.invalidate(index);
  paths[index] = widths[index] > 0 && heights[index] > 0
                     ? UITheme::getCoverThumbPath((*books)[index].coverBmpPath, widths[index], heights[index], false)
                     : std::string();
}

int HomeCoverSlots::widthFor(const size_t index) const {
  return index < widths.size() && widths[index] > 0 ? widths[index] : DEFAULT_HEIGHT * 2 / 3;
}

int HomeCoverSlots::heightFor(const size_t index) const {
  return index < heights.size() && heights[index] > 0 ? heights[index] : DEFAULT_HEIGHT;
}

bool HomeCoverSlots::takeSizesChanged() { return std::exchange(sizesChanged, false); }

void HomeCoverSlots::note(const size_t index, const int slotWidth, const int slotHeight) {
  if (index >= heights.size()) return;
  const int width = std::max(1, slotWidth);
  const int height = std::max(1, slotHeight);
  if (widths[index] != width || heights[index] != height) {
    widths[index] = width;
    heights[index] = height;
    sizesChanged = true;
    refreshPath(index);
  }
}

bool HomeCoverSlots::paintFramed(fui::DrawTarget& target, const fui::Rect rect, const size_t index) {
  constexpr int16_t SHADOW_OFFSET = 2;
  const auto ink = fui::Paint::solid(fui::Color::Black);
  target.fill(fui::Rect{rect.right(), static_cast<int16_t>(rect.y + SHADOW_OFFSET), SHADOW_OFFSET, rect.height}, ink);
  target.fill(fui::Rect{static_cast<int16_t>(rect.x + SHADOW_OFFSET), rect.bottom(), rect.width, SHADOW_OFFSET}, ink);
  // The cache draws the no-cover placeholder itself and snapshots it with the covers.
  const bool drawn =
      books && index < paths.size() && index < books->size() && cache.paint(rect, index, paths[index], (*books)[index]);
  target.stroke(rect, ink, 1, 0);
  return drawn;
}
