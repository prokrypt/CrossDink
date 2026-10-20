#include "I18n.h"

#include <Logging.h>
#include <PackedAsset.h>

#include <atomic>
#include <cstddef>
#include <cstring>

#include "I18nStrings.h"

using namespace i18n_strings;

namespace {
bool isBuiltinLanguage(const Language language) {
  const auto raw = static_cast<uint8_t>(language);
  for (const uint8_t builtin : SORTED_LANGUAGE_INDICES) {
    if (builtin == raw) return true;
  }
  return false;
}

// English lives raw in flash; other languages are inflated from their pack
// into PSRAM when selected (~10-24 KB each instead of ~440 KB in flash).
constexpr LangStrings kEnglish{STRINGS_EN_DATA, OFFSETS_EN};

// get() can run on the render task while setLanguage() runs on the main loop,
// so the active table is published as one pointer. The LangStrings header sits
// at the front of each pack buffer, so the pointer and its data swap together.
std::atomic<const LangStrings*> activeStrings{&kEnglish};

// The previous pack outlives one more switch: a tr() pointer fetched just
// before a switch may still be drawn after it.
HeapByteBuffer currentPack;
HeapByteBuffer previousPack;

// Header room in front of the inflated pack; keeps the uint16 offsets aligned.
constexpr size_t kPackHeader = (sizeof(LangStrings) + 7) & ~size_t{7};

const LangStrings* loadPack(const Language lang) {
  const PackedLanguage& packed = PACKED_LANGUAGES[static_cast<size_t>(lang)];
  if (!packed.data) return &kEnglish;
  constexpr size_t offsetBytes = static_cast<size_t>(StrId::_COUNT) * sizeof(uint16_t);
  if (packed.rawSize <= offsetBytes) {
    LOG_ERR("I18N", "Language pack %u too small", unsigned(lang));
    return nullptr;
  }
  HeapByteBuffer buf = inflatePackedAsset({packed.data, packed.packedSize, packed.rawSize}, "I18N",
                                          PackedAssetFallback::PsramOnly, kPackHeader);
  if (!buf) return nullptr;
  auto* table = reinterpret_cast<LangStrings*>(buf.get());
  table->offsets = reinterpret_cast<const uint16_t*>(buf.get() + kPackHeader);
  table->data = reinterpret_cast<const char*>(buf.get() + kPackHeader + offsetBytes);
  previousPack = std::move(currentPack);
  currentPack = std::move(buf);
  return table;
}
}  // namespace

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= static_cast<size_t>(StrId::_COUNT)) {
    return "???";
  }

  const LangStrings* lang = activeStrings.load(std::memory_order_acquire);

  // If bit 15 of the offset is set, apply the offset to the English lookup table
  const uint16_t off = lang->offsets[index];
  if (off & 0x8000) return STRINGS_EN_DATA + (off & 0x7FFF);
  return lang->data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  // Keep persisted settings untouched, but make every runtime language-dependent
  // behavior agree with the English string fallback in reduced-language builds.
  Language effective = isBuiltinLanguage(lang) ? lang : Language::EN;
  if (effective == _language && (effective == Language::EN || currentPack)) return;
  const LangStrings* table = loadPack(effective);
  if (!table) {
    // Same English fallback as a reduced-language build.
    LOG_ERR("I18N", "Language %u unavailable; using English", unsigned(effective));
    effective = Language::EN;
    table = &kEnglish;
  }
  activeStrings.store(table, std::memory_order_release);
  _language = effective;
}

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(code, LANGUAGE_CODES[i]) == 0) return static_cast<Language>(i);
  }
  return Language::EN;
}

// Generate character set for a specific language
const char* I18n::getCharacterSet(Language lang) {
  const auto langIndex = static_cast<size_t>(lang);
  if (langIndex >= static_cast<size_t>(Language::_COUNT)) {
    lang = Language::EN;  // Fallback to first language
  }

  return CHARACTER_SETS[static_cast<size_t>(lang)];
}
