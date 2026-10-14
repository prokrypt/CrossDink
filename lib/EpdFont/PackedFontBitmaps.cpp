#include "PackedFontBitmaps.h"

#include <Logging.h>

#include <new>
#include <type_traits>

static_assert(std::is_trivially_copyable_v<EpdFontData> && std::is_trivially_destructible_v<EpdFontData>,
              "EpdFontData is placement-copied into the bitmap buffer and never destroyed");

bool attachPackedBitmaps(EpdFont& font, const PackedAsset& bitmaps, HeapByteBuffer& storage) {
  if (storage) return true;
  // The patched EpdFontData sits in front of the bitmaps in one allocation, so
  // the font costs no internal RAM. Size: bitmaps.rawSize (10-45 KB per UI
  // font) plus this header; lives as long as the font is registered.
  constexpr size_t header = (sizeof(EpdFontData) + 7) & ~size_t{7};
  HeapByteBuffer buffer = inflatePackedAsset(bitmaps, "FONT", PackedAssetFallback::PsramThenDefault, header);
  if (!buffer) {
    LOG_ERR("FONT", "Cannot unpack font bitmaps (%u bytes); its text will draw blank", unsigned(bitmaps.rawSize));
    return false;
  }
  auto* data = new (buffer.get()) EpdFontData(*font.data);
  data->bitmap = buffer.get() + header;
  font.data = data;
  storage = std::move(buffer);
  return true;
}
