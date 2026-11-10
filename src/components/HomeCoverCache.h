#pragma once

#include <Bitmap.h>
#include <FreeInkUICore.h>
#include <HalStorage.h>
#include <Memory.h>

#include <array>
#include <string>
#include <vector>

class GfxRenderer;
struct RecentBook;

// Screen-lifetime thumbnail decoding and PSRAM region snapshots.
class HomeCoverCache {
 public:
  static constexpr size_t MAX_COVERS = 7;
  explicit HomeCoverCache(GfxRenderer& renderer) : renderer(renderer) {}
  void begin();
  void prepare();
  void invalidate();
  void invalidate(size_t index);
  bool paint(freeink::ui::Rect rect, size_t index, const std::string& path, const RecentBook& book);

 private:
  struct CachedCover {
    freeink::ui::Rect rect{};
    size_t offset = 0;
    size_t bytes = 0;
    bool valid = false;
  };
  HeapByteBuffer coverCache;
  std::array<CachedCover, MAX_COVERS> cachedCovers{};
  size_t coverCacheCapacity = 0;
  size_t coverCacheUsed = 0;
  int coverCacheOrientation = -1;
  bool coverCacheInverted = false;

  HalFile coverFile;
  Bitmap coverBitmap{coverFile};
  GfxRenderer& renderer;
};

// The recent books' cover slots of a Home that lays its covers out itself (Cover
// Grid, a theme pack's Home): the size each slot draws at, noted while drawing,
// and the thumb made at exactly that size (rescaling a dithered 1-bit image
// aliases badly). Home's cover worker makes the missing thumbs.
class HomeCoverSlots {
 public:
  static constexpr int DEFAULT_HEIGHT = 400;
  explicit HomeCoverSlots(GfxRenderer& renderer) : cache(renderer) {}
  void begin(const std::vector<RecentBook>& books);
  void refreshPaths();
  void prepare() { cache.prepare(); }
  // A changed size re-points the slot at its new thumb; takeSizesChanged() reports it.
  void note(size_t index, int width, int height);
  int widthFor(size_t index) const;
  int heightFor(size_t index) const;
  bool takeSizesChanged();
  // The book's cover, or the no-cover placeholder, in rect, with a 1 px frame and
  // a 2 px drop shadow.
  bool paintFramed(freeink::ui::DrawTarget& target, freeink::ui::Rect rect, size_t index);

 private:
  void refreshPath(size_t index);

  HomeCoverCache cache;
  const std::vector<RecentBook>* books = nullptr;
  std::array<std::string, HomeCoverCache::MAX_COVERS> paths;
  std::array<int, HomeCoverCache::MAX_COVERS> widths{};
  std::array<int, HomeCoverCache::MAX_COVERS> heights{};
  bool sizesChanged = false;
};
