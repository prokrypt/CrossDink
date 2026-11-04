#pragma once
#include <Memory.h>

#include <cstdint>

// Host stub: built-in packs are not inflated, so a non-English built-in language
// falls back to English exactly like a reduced-language build.
struct PackedAsset {
  const uint8_t* data;
  uint32_t packedSize;
  uint32_t rawSize;
};
enum class PackedAssetFallback : uint8_t { PsramOnly, PsramThenDefault };
inline HeapByteBuffer inflatePackedAsset(const PackedAsset&, const char*,
                                         PackedAssetFallback = PackedAssetFallback::PsramOnly, size_t = 0) {
  return {};
}
