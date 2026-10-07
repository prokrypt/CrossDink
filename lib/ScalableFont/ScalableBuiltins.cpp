#include "ScalableBuiltins.h"
#if CROSSDINK_SCALABLE_FONTS
#include <GfxRenderer.h>
#include <Logging.h>
#include <PackedAsset.h>

#include <optional>

#include "../../src/ReaderFontSizeStep.h"
#include "../../src/fontIds.h"
#include "HalScalableFont.h"
#include "ScalableAssets.generated.h"
namespace {
std::optional<HalScalableFont> faces[8];
bool ready[2] = {};
// Packed TTFs in flash, inflated into PSRAM when their family is first used.
const PackedAsset assets[] = {
    {lexenddeca_regularPacked, sizeof(lexenddeca_regularPacked), lexenddeca_regularRawSize},
    {lexenddeca_boldPacked, sizeof(lexenddeca_boldPacked), lexenddeca_boldRawSize},
    {bitter_regularPacked, sizeof(bitter_regularPacked), bitter_regularRawSize},
    {bitter_boldPacked, sizeof(bitter_boldPacked), bitter_boldRawSize},
    {bitter_italicPacked, sizeof(bitter_italicPacked), bitter_italicRawSize},
    {bitter_bolditalicPacked, sizeof(bitter_bolditalicPacked), bitter_bolditalicRawSize},
};
constexpr size_t AssetCount = sizeof(assets) / sizeof(assets[0]);
// Inflated TTF bytes; FreeType borrows them for as long as the faces live.
// 123 KB (Lexend Deca) / 421 KB (Bitter) of PSRAM per family in use: too big
// and too long-lived for anything but the heap.
HeapByteBuffer assetBytes[AssetCount];
// Lexend Deca has no designed italic; the files the firmware used to embed
// were the upright faces sheared by 0.2 (11.3 degrees). The same shear on the
// upright bytes renders within ~1% of their pixels at reading sizes.
constexpr int32_t LexendObliqueShear16_16 = 13107;  // 0.2 in 16.16
struct BuiltinFace {
  uint8_t asset;
  int32_t slant16_16;
};
// [family][regular, bold, italic, bold-italic]
constexpr BuiltinFace faceSources[2][4] = {
    {{0, 0}, {1, 0}, {0, LexendObliqueShear16_16}, {1, LexendObliqueShear16_16}},
    {{2, 0}, {3, 0}, {4, 0}, {5, 0}},
};
constexpr int ids[2][4] = {{LEXENDDECA_10_FONT_ID, LEXENDDECA_12_FONT_ID, LEXENDDECA_14_FONT_ID, LEXENDDECA_16_FONT_ID},
                           {BITTER_10_FONT_ID, BITTER_12_FONT_ID, BITTER_14_FONT_ID, BITTER_16_FONT_ID}};
}  // namespace
int scalableBuiltinFontId(int id) {
  for (unsigned f = 0; f < 2; ++f)
    for (unsigned s = 0; s < 4; ++s)
      if (ids[f][s] == id && !ready[f]) return UI_12_FONT_ID;
  const int result = int(uint32_t(id) ^ OutlineFingerprint ^ 0x53544600u ^ HalScalableFont::renderingRevision());
  return result ? result : 1;
}
int scalableBuiltinReaderFontId(unsigned family, unsigned points) {
  if (family >= 2 || !ready[family]) return UI_12_FONT_ID;
  // Preserve existing cache IDs for the original four sizes.
  for (unsigned s = 0; s < 4; ++s)
    if (points == 10 + s * 2) return scalableBuiltinFontId(ids[family][s]);
  return scalableBuiltinFontId(0x54540000 | (family << 8) | points);
}
void ensureScalableBuiltinFamily(GfxRenderer& renderer, unsigned family) {
  ScalableFontAccess access;
  if (family >= 2 || ready[family]) return;
  bool ok = true;
  for (unsigned style = 0; style < 4 && ok; ++style) {
    const BuiltinFace& source = faceSources[family][style];
    HeapByteBuffer& bytes = assetBytes[source.asset];
    if (!bytes) bytes = inflatePackedAsset(assets[source.asset], "TTF");
    if (!bytes) {
      ok = false;
      break;
    }
    freeink::font::FtFont::RenderOptions options;
    options.hinting = freeink::font::FtFont::HintingMode::Auto;
    options.slant16_16 = source.slant16_16;
    unsigned i = family * 4 + style;
    faces[i].emplace();
    ok = faces[i]->openMemory(bytes.get(), assets[source.asset].rawSize, options);
  }
  if (!ok) {
    // Faces borrow the bytes (Lexend's italics share the upright files), so
    // close every face before freeing any of them.
    for (unsigned style = 0; style < 4; ++style) faces[family * 4 + style].reset();
    for (unsigned style = 0; style < 4; ++style) assetBytes[faceSources[family][style].asset].reset();
    LOG_ERR("TTF", "Built-in family unavailable; using UI recovery font");
  }
  ready[family] = ok;
  for (uint8_t points : SCALABLE_READER_FONT_SIZES) {
    if (ok) {
      EpdFontFamily font(faces[family * 4]->atSize(points), faces[family * 4 + 1]->atSize(points),
                         faces[family * 4 + 2]->atSize(points), faces[family * 4 + 3]->atSize(points));
      renderer.insertFont(scalableBuiltinReaderFontId(family, points), font);
      for (unsigned size = 0; size < 4; ++size)
        if (points == 10 + size * 2) renderer.insertFont(ids[family][size], font);
    } else {
      const auto it = renderer.getFontMap().find(UI_12_FONT_ID);
      if (it != renderer.getFontMap().end()) {
        for (unsigned size = 0; size < 4; ++size)
          if (points == 10 + size * 2) renderer.insertFont(ids[family][size], it->second);
      }
    }
  }
}
#endif
