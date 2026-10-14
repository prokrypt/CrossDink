#pragma once

#include <Memory.h>

#include <cstddef>
#include <cstdint>

// A read-only asset stored in flash as a raw DEFLATE stream (no zlib header),
// produced at build time by a generator script (zlib level 9, wbits -15).
//
// Only one piece of most embedded assets is in use at a time (one UI language,
// one reader font family, one hyphenation language), so the image keeps them
// compressed and the app inflates the active piece into PSRAM on first use.
// The inflated copy is plain CPU data: never pass it to DMA or ISR code.
struct PackedAsset {
  const uint8_t* data;  // DEFLATE stream in flash
  uint32_t packedSize;  // bytes at data
  uint32_t rawSize;     // bytes after inflating
};

enum class PackedAssetFallback : uint8_t {
  // PSRAM only. Callers that can degrade (English strings, the UI recovery
  // font, no hyphenation) use this so a big asset never eats internal RAM.
  PsramOnly,
  // PSRAM first, then the default heap. For assets the UI cannot run without.
  PsramThenDefault,
};

// Inflate `asset` into a new buffer of asset.rawSize bytes (plus `headroom`
// zeroed bytes in front, for a caller-owned header). Returns an empty buffer
// and logs on allocation or stream failure. The simulator has no PSRAM, so
// there both policies use the default heap.
HeapByteBuffer inflatePackedAsset(const PackedAsset& asset, const char* tag,
                                  PackedAssetFallback fallback = PackedAssetFallback::PsramOnly, size_t headroom = 0);
