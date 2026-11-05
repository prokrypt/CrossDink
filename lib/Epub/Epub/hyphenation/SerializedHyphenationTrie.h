#pragma once

#include <cstddef>
#include <cstdint>

// Lightweight descriptor that points at a serialized Liang hyphenation trie in memory.
struct SerializedHyphenationPatterns {
  size_t rootOffset;
  const std::uint8_t* data;
  size_t size;
};

// A trie as stored in flash: raw DEFLATE (PackedAsset), inflated on first use.
struct PackedHyphenationPatterns {
  size_t rootOffset;
  const std::uint8_t* packed;
  size_t packedSize;
  size_t size;  // inflated trie bytes
};
