#pragma once

#include <Memory.h>

#include "LiangHyphenation.h"

// Generic Liang-backed hyphenator that stores pattern metadata plus language-specific helpers.
// The trie ships packed in flash; LanguageRegistry calls load() before handing the hyphenator out.
class LanguageHyphenator {
 public:
  LanguageHyphenator(const PackedHyphenationPatterns& packed, bool (*isLetterFn)(uint32_t),
                     uint32_t (*toLowerFn)(uint32_t), size_t minPrefix = LiangWordConfig::kDefaultMinPrefix,
                     size_t minSuffix = LiangWordConfig::kDefaultMinSuffix)
      : packed_(packed), config_(isLetterFn, toLowerFn, minPrefix, minSuffix) {}

  // Inflate the trie into PSRAM on first use; it then stays resident (at most
  // 206 KB, German). Returns false if it cannot be loaded. Not thread-safe:
  // LanguageRegistry serializes calls.
  bool load();

  std::vector<size_t> breakIndexes(const std::vector<CodepointInfo>& cps) const {
    if (!patterns_.data) return {};
    return liangBreakIndexes(cps, patterns_, config_);
  }

  size_t minPrefix() const { return config_.minPrefix; }
  size_t minSuffix() const { return config_.minSuffix; }

 protected:
  const PackedHyphenationPatterns& packed_;
  SerializedHyphenationPatterns patterns_{};
  HeapByteBuffer trie_;
  LiangWordConfig config_;
};
