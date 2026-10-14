#include "LanguageRegistry.h"

#include <Logging.h>
#include <PackedAsset.h>

#include <algorithm>
#include <array>
#include <mutex>

#include "HyphenationCommon.h"
#include "generated/hyph-de.trie.h"
#include "generated/hyph-en.trie.h"
#include "generated/hyph-es.trie.h"
#include "generated/hyph-fr.trie.h"
#include "generated/hyph-it.trie.h"
#include "generated/hyph-pl.trie.h"
#include "generated/hyph-pt.trie.h"
#include "generated/hyph-ru.trie.h"
#include "generated/hyph-sv.trie.h"
#include "generated/hyph-uk.trie.h"

namespace {

// English hyphenation patterns (3/3 minimum prefix/suffix length)
LanguageHyphenator englishHyphenator(en_patterns, isLatinLetter, toLowerLatin, 3, 3);
LanguageHyphenator frenchHyphenator(fr_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator germanHyphenator(de_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator russianHyphenator(ru_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator spanishHyphenator(es_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator italianHyphenator(it_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator swedishHyphenator(sv_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator ukrainianHyphenator(uk_patterns, isCyrillicLetter, toLowerCyrillic);
LanguageHyphenator polishHyphenator(pl_patterns, isLatinLetter, toLowerLatin);
LanguageHyphenator portugueseHyphenator(pt_patterns, isLatinLetter, toLowerLatin);

using EntryArray = std::array<LanguageEntry, 10>;

// Section builds and previews may look a language up from different tasks.
std::mutex& loadMutex() {
  static std::mutex mutex;
  return mutex;
}

const EntryArray& entries() {
  static const EntryArray kEntries = {{{"english", "en", &englishHyphenator},
                                       {"french", "fr", &frenchHyphenator},
                                       {"german", "de", &germanHyphenator},
                                       {"russian", "ru", &russianHyphenator},
                                       {"spanish", "es", &spanishHyphenator},
                                       {"italian", "it", &italianHyphenator},
                                       {"polish", "pl", &polishHyphenator},
                                       {"portuguese", "pt", &portugueseHyphenator},
                                       {"swedish", "sv", &swedishHyphenator},
                                       {"ukrainian", "uk", &ukrainianHyphenator}}};
  return kEntries;
}

}  // namespace

bool LanguageHyphenator::load() {
  if (patterns_.data) return true;
  // Plain CPU data read by the Liang walk; PSRAM only, since a missing trie
  // just means no hyphenation for this language.
  trie_ = inflatePackedAsset(
      {packed_.packed, static_cast<uint32_t>(packed_.packedSize), static_cast<uint32_t>(packed_.size)}, "HYPH");
  if (!trie_) return false;
  patterns_ = {packed_.rootOffset, trie_.get(), packed_.size};
  return true;
}

const LanguageHyphenator* getLanguageHyphenatorForPrimaryTag(const std::string& primaryTag) {
  const auto& allEntries = entries();
  const auto it = std::find_if(allEntries.begin(), allEntries.end(),
                               [&primaryTag](const LanguageEntry& entry) { return primaryTag == entry.primaryTag; });
  if (it == allEntries.end()) return nullptr;
  std::lock_guard<std::mutex> lock(loadMutex());
  if (!it->hyphenator->load()) {
    LOG_ERR("HYPH", "Hyphenation patterns for '%s' unavailable", it->primaryTag);
    return nullptr;
  }
  return it->hyphenator;
}

LanguageEntryView getLanguageEntries() {
  const auto& allEntries = entries();
  return LanguageEntryView{allEntries.data(), allEntries.size()};
}
