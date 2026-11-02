#include "I18n.h"

#include <HalStorage.h>
#include <Logging.h>
#include <PackedAsset.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>

#include "I18nStrings.h"

#if defined(SIMULATOR)
#include <chrono>
#else
#include <esp_timer.h>
#endif

using namespace i18n_strings;
using namespace language_cache;

namespace {
constexpr size_t KEY_COUNT = static_cast<size_t>(StrId::_COUNT);
static_assert(KEY_COUNT <= MAX_KEYS, "LanguageCache key limit");
static_assert(MISSING == I18N_ENGLISH_FALLBACK_OFFSET, "packs and SD caches share the English fallback offset");
// Two A/B slots, the same layout LanguageCache uses in a flash partition, in one SD file.
constexpr size_t CACHE_SIZE = 2 * SLOT_SIZE;
constexpr size_t MAX_FILENAME = 128;

Key keyAt(size_t i) {
  const auto& entry = TRANSLATION_KEYS[i];
  return {entry.keyHash, entry.englishHash, entry.name, entry.id, entry.formatted};
}
const char* english(uint16_t id) { return STRINGS_EN_DATA + OFFSETS_EN[id]; }
const Schema SCHEMA{KEY_COUNT, keyAt, english};

bool isBuiltinLanguage(const Language language) {
  const auto raw = static_cast<uint8_t>(language);
  for (const uint8_t builtin : SORTED_LANGUAGE_INDICES) {
    if (builtin == raw) return true;
  }
  return false;
}

uint64_t microseconds() {
#if defined(SIMULATOR)
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
#else
  return esp_timer_get_time();
#endif
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
constexpr size_t kOffsetBytes = KEY_COUNT * sizeof(uint16_t);
// SD language table: [LangStrings][uint16 offsets][64 KiB slot image].
constexpr size_t kSlotOffsetsEnd = kPackHeader + ((kOffsetBytes + 7) & ~size_t{7});
constexpr size_t kSlotTableBytes = kSlotOffsetsEnd + SLOT_SIZE;

void publish(const LangStrings* table, HeapByteBuffer buffer) {
  previousPack = std::move(currentPack);
  currentPack = std::move(buffer);
  activeStrings.store(table, std::memory_order_release);
}

// 64 KiB buffers: PSRAM on the device, plain heap on the simulator only.
HeapByteBuffer allocateLarge(const size_t bytes) {
  return psramHeapAvailable() ? makePsramByteBufferNoThrow(bytes) : makeHeapByteBufferNoThrow(bytes);
}

const LangStrings* loadPack(const Language lang) {
  const PackedLanguage& packed = PACKED_LANGUAGES[static_cast<size_t>(lang)];
  if (!packed.data) return &kEnglish;
  if (packed.rawSize <= kOffsetBytes) {
    LOG_ERR("I18N", "Language pack %u too small", unsigned(lang));
    return nullptr;
  }
  HeapByteBuffer buf = inflatePackedAsset({packed.data, packed.packedSize, packed.rawSize}, "I18N",
                                          PackedAssetFallback::PsramOnly, kPackHeader);
  if (!buf) return nullptr;
  auto* table = reinterpret_cast<LangStrings*>(buf.get());
  table->offsets = reinterpret_cast<const uint16_t*>(buf.get() + kPackHeader);
  table->data = reinterpret_cast<const char*>(buf.get() + kPackHeader + kOffsetBytes);
  publish(table, std::move(buf));
  return table;
}

int readInput(void* context, void* bytes, size_t count) { return static_cast<HalFile*>(context)->read(bytes, count); }
bool rewindInput(void* context) { return static_cast<HalFile*>(context)->seek(0); }

// The cache file as LanguageCache's Flash: one handle for the whole install.
// erase() writes 0xff like erased flash so LanguageCache's commit rules hold.
struct CacheFile {
  HalFile file;

  bool open() {
    if (!Storage.exists(I18n::CACHE_FILE)) {
      if (!Storage.ensureDirectoryExists(DIRECTORY)) {
        LOG_ERR("LANG", "Cannot create %s", DIRECTORY);
        return false;
      }
      file = Storage.open(I18n::CACHE_FILE, O_RDWR | O_CREAT);
      if (!file) {
        LOG_ERR("LANG", "Cannot create language cache");
        file.close();
        return false;
      }
      if (fill(0, CACHE_SIZE)) return true;
      LOG_ERR("LANG", "Cannot initialize language cache");
      file.close();
      if (!Storage.remove(I18n::CACHE_FILE)) LOG_ERR("LANG", "Cannot remove incomplete language cache");
      return false;
    }
    file = Storage.open(I18n::CACHE_FILE, O_RDWR);
    if (!file || file.fileSize() != CACHE_SIZE) {
      LOG_ERR("LANG", "Cannot open language cache or unexpected size");
      file.close();
      return false;
    }
    return true;
  }
  bool close() { return file.close(); }

  bool fill(size_t offset, size_t length) {
    uint8_t erased[256];
    std::memset(erased, 0xff, sizeof(erased));
    bool ok = file.seek(offset);
    for (size_t done = 0; ok && done < length; done += sizeof(erased)) {
      const size_t n = std::min(sizeof(erased), length - done);
      ok = file.write(erased, n) == n;
    }
    return ok && file.sync();
  }
  static bool read(void* context, size_t offset, void* bytes, size_t length) {
    auto& file = static_cast<CacheFile*>(context)->file;
    return file.seek(offset) && file.read(bytes, length) == static_cast<int>(length);
  }
  static bool write(void* context, size_t offset, const void* bytes, size_t length) {
    auto& file = static_cast<CacheFile*>(context)->file;
    return file.seek(offset) && file.write(bytes, length) == length && file.sync();
  }
  static bool erase(void* context, size_t offset, size_t length) {
    return static_cast<CacheFile*>(context)->fill(offset, length);
  }
  static bool verify(void* context, size_t offset, const void* bytes, size_t length) {
    uint8_t check[256];
    const auto* expected = static_cast<const uint8_t*>(bytes);
    for (size_t done = 0; done < length; done += sizeof(check)) {
      const size_t count = std::min(sizeof(check), length - done);
      if (!read(context, offset + done, check, count) || std::memcmp(check, expected + done, count)) {
        LOG_ERR("LANG", "Language cache readback failed");
        return false;
      }
    }
    return true;
  }
};
}  // namespace

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

const char* I18n::get(StrId id) const {
  const auto index = static_cast<size_t>(id);
  if (index >= KEY_COUNT) {
    return "???";
  }

  const LangStrings* lang = activeStrings.load(std::memory_order_acquire);

  const uint16_t off = lang->offsets[index];
  if (off == I18N_ENGLISH_FALLBACK_OFFSET) return english(static_cast<uint16_t>(index));
  return lang->data + off;
}

void I18n::setLanguage(Language lang) {
  if (lang >= Language::_COUNT) {
    return;
  }
  // Keep persisted settings untouched, but make every runtime language-dependent
  // behavior agree with the English string fallback in reduced-language builds.
  Language effective = isBuiltinLanguage(lang) ? lang : Language::EN;
  if (slot_ < 0 && effective == _language && (effective == Language::EN || currentPack)) return;
  const LangStrings* table = effective == Language::EN ? &kEnglish : loadPack(effective);
  if (!table) {
    // Same English fallback as a reduced-language build.
    LOG_ERR("I18N", "Language %u unavailable; using English", unsigned(effective));
    effective = Language::EN;
    table = &kEnglish;
  }
  if (table == &kEnglish) publish(table, {});
  publishBuiltin(effective);
}

void I18n::publishBuiltin(Language lang) {
  _language = lang;
  std::strcpy(code_, LANGUAGE_CODES[static_cast<uint8_t>(lang)]);
  name_[0] = '\0';
  generation_ = 0;
  slot_ = -1;
}

void I18n::begin(const char* code, uint64_t generation) {
  if (!code || !*code) code = "EN";
  if (std::strcmp(code, "EN") == 0) {
    setLanguage(Language::EN);
    return;
  }
  // Two 256-byte header reads pick the slot; only that slot is then read.
  HalFile cache;
  int best = -1;
  uint64_t bestGeneration = 0;
  if (Storage.openFileForReadIfPresent("LANG", CACHE_FILE, cache)) {
    for (int slot = 0; slot < 2; ++slot) {
      uint8_t header[HEADER_SIZE];
      Installed info;
      if (cache.seek(slotOffset(CACHE_SIZE, slot)) && cache.read(header, sizeof(header)) == int(sizeof(header)) &&
          peek(header, info) && std::strcmp(info.metadata.code, code) == 0 &&
          (!generation || info.generation == generation) && info.generation > bestGeneration) {
        best = slot;
        bestGeneration = info.generation;
      }
    }
  }
  cache.close();
  if (best >= 0 && activateSlot(best, generation)) return;
  if (best >= 0) LOG_ERR("LANG", "Cached language %s unavailable; using the built-in pack", code);
  setLanguage(languageFromCode(code));
}

bool I18n::activateSlot(int slot, uint64_t generation) {
  const uint64_t start = microseconds();
  HeapByteBuffer buf = allocateLarge(kSlotTableBytes);
  if (!buf) {
    LOG_ERR("LANG", "Cannot allocate %u bytes for the SD language", unsigned(kSlotTableBytes));
    return false;
  }
  uint8_t* bytes = buf.get() + kSlotOffsetsEnd;
  HalFile cache;
  const bool read = Storage.openFileForRead("LANG", CACHE_FILE, cache) && cache.seek(slotOffset(CACHE_SIZE, slot)) &&
                    cache.read(bytes, SLOT_SIZE) == static_cast<int>(SLOT_SIZE);
  cache.close();
  if (!read) {
    LOG_ERR("LANG", "Cannot read language slot %d", slot);
    return false;
  }
  auto* offsets = reinterpret_cast<uint16_t*>(buf.get() + kPackHeader);
  Installed installed;
  if (!language_cache::open(bytes, SLOT_SIZE, SCHEMA, installed, offsets) ||
      (generation && installed.generation != generation)) {
    LOG_ERR("LANG", "Language slot %d is damaged or stale", slot);
    return false;
  }
  auto* table = reinterpret_cast<LangStrings*>(buf.get());
  table->offsets = offsets;
  table->data = reinterpret_cast<const char*>(bytes);
  publish(table, std::move(buf));
  _language = languageFromCode(installed.metadata.keyboard);
  std::strcpy(code_, installed.metadata.code);
  std::strcpy(name_, installed.metadata.name);
  generation_ = installed.generation;
  slot_ = slot;
  LOG_INF("LANG", "Loaded %s from SD slot %d in %llu us, generation=%llu", code_, slot,
          static_cast<unsigned long long>(microseconds() - start), static_cast<unsigned long long>(generation_));
  return true;
}

const char* I18n::getName() const { return name_[0] ? name_ : LANGUAGE_NAMES[static_cast<uint8_t>(_language)]; }

const char* I18n::getLanguageName(Language lang) const {
  const auto index = static_cast<size_t>(lang);
  if (index >= static_cast<size_t>(Language::_COUNT)) {
    return "???";
  }
  return LANGUAGE_NAMES[index];
}

Language I18n::languageFromCode(const char* code) {
  if (code)
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

static_assert(sizeof(I18n::Catalog) < 3072);
void I18n::Catalog::reset() {
  count = 0;
  used = 1;
  text[0] = '\0';
  cachedGeneration = 0;
}
uint16_t I18n::Catalog::append(const char* value) {
  if (!*value) return 0;
  const auto offset = static_cast<uint16_t>(used);
  const size_t n = std::strlen(value) + 1;
  std::memcpy(text + used, value, n);
  used += n;
  return offset;
}
bool I18n::Catalog::add(const char* code, const char* name, const char* filename, bool cached) {
  const size_t bytes = std::strlen(code) + std::strlen(name) + std::strlen(filename) + 3;
  if (count == CAPACITY || bytes > TEXT_BYTES - used) return false;
  entries[count++] = {append(code), append(name), append(filename), false, cached};
  return true;
}
bool I18n::Catalog::source(size_t index, const char* name, const char* filename) {
  const size_t bytes = std::strlen(name) + std::strlen(filename) + 2;
  if (bytes > TEXT_BYTES - used) return false;
  entries[index].name = append(name);
  entries[index].file = append(filename);
  return true;
}
void I18n::Catalog::select(size_t index, Option& result) const {
  if (index >= count) return;
  std::strcpy(result.code, code(index));
  std::strcpy(result.name, name(index));
  result.path[0] = '\0';
  if (*file(index)) std::snprintf(result.path, sizeof(result.path), "%s/%s", DIRECTORY, file(index));
  result.generation = entries[index].cached ? cachedGeneration : 0;
  result.duplicate = entries[index].duplicate;
}

Result I18n::discover(Catalog& catalog) const {
  catalog.reset();
  bool cachedAdded = false;
  for (uint8_t index = 0; index < getLanguageCount(); ++index) {
    if (!isBuiltinLanguage(static_cast<Language>(index))) continue;
    const bool cached = slot_ >= 0 && std::strcmp(code_, LANGUAGE_CODES[index]) == 0;
    if (!catalog.add(LANGUAGE_CODES[index], cached ? name_ : LANGUAGE_NAMES[index], "", cached))
      return Result::TooLarge;
    cachedAdded |= cached;
  }
  if (slot_ >= 0 && !cachedAdded && !catalog.add(code_, name_, "", true)) return Result::TooLarge;
  catalog.cachedGeneration = generation_;
  auto addSource = [&](const Metadata& metadata, const char* filename) {
    size_t existing = 1;
    while (existing < catalog.count && std::strcmp(catalog.code(existing), metadata.code) != 0) ++existing;
    if (existing < catalog.count) {
      if (*catalog.file(existing)) {
        catalog.entries[existing].duplicate = true;
        LOG_ERR("LANG", "Duplicate language code %s", metadata.code);
        return true;
      }
      return catalog.source(existing, metadata.name, filename);
    }
    return catalog.add(metadata.code, metadata.name, filename);
  };
  auto directory = Storage.open(DIRECTORY);
  if (!directory || !directory.isDirectory()) {
    const bool allocationFailed = directory.allocationFailed();
    directory.close();
    return allocationFailed ? Result::Memory : Result::Ok;  // No SD languages yet.
  }
  Inspector inspector;
  if (!inspector.available()) {
    LOG_ERR("LANG", "OOM: language metadata workspace");
    directory.close();
    return Result::Memory;
  }
  Result status = Result::Ok;
  size_t files = 0;
  while (true) {
    auto file = directory.openNextFile();
    if (!file) {
      if (directory.allocationFailed())
        status = Result::Memory;
      else if (directory.iterationFailed())
        status = Result::Io;
      file.close();
      break;
    }
    char filename[MAX_FILENAME];
    const size_t nameLength = file.getName(filename, sizeof(filename));
    if (file.isDirectory() || !nameLength || nameLength >= sizeof(filename) - 1 || isAppleDoubleFile(filename)) {
      file.close();
      continue;
    }
    const size_t length = std::strlen(filename);
    if (length < 6 || std::strcmp(filename + length - 5, ".yaml") != 0) {
      file.close();
      continue;
    }
    if (++files > Catalog::MAX_SOURCE_FILES) {
      status = Result::TooLarge;
      file.close();
      break;
    }
    Metadata metadata;
    const Result result =
        file.fileSize() > MAX_SOURCE_SIZE ? Result::TooLarge : inspector.inspect({&file, readInput}, metadata);
    file.close();
    if (result != Result::Ok) {
      LOG_ERR("LANG", "%s: %s", filename, resultName(result));
      continue;
    }
    if (!addSource(metadata, filename)) {
      status = Result::TooLarge;
      break;
    }
  }
  directory.close();
  if (status != Result::Ok) {
    LOG_ERR("LANG", "Cannot show language catalog: %s (limit=%u text bytes, %u files)", resultName(status),
            unsigned(Catalog::TEXT_BYTES), unsigned(Catalog::MAX_SOURCE_FILES));
    return status;
  }
  std::sort(catalog.entries + 1, catalog.entries + catalog.count,
            [&catalog](const Catalog::Entry& a, const Catalog::Entry& b) {
              return std::strcmp(catalog.text + a.name, catalog.text + b.name) < 0;
            });
  return Result::Ok;
}

const char* I18n::sourceContract() { return SOURCE_SHA256; }
const char* I18n::keysetContract() { return KEYSET_SHA256; }
bool I18n::supportedDownload(const char* code) {
  if (!code) return false;
  for (size_t i = 1; i < getLanguageCount(); ++i)
    if (!std::strcmp(code, LANGUAGE_CODES[i])) return true;
  return false;
}

Result I18n::validateSource(const char* path, const char* code, const char* source, const char* keyset, void* context,
                            bool (*continueWork)(void*)) const {
  if (!supportedDownload(code) || !source || !keyset || std::strcmp(source, sourceContract()) ||
      std::strcmp(keyset, keysetContract())) {
    LOG_ERR("LANG", "Download source contract mismatch");
    return Result::Invalid;
  }
  HalFile file;
  if (!Storage.openFileForRead("LANG", path, file)) return Result::Io;
  Metadata metadata;
  ValidationOptions options;
  options.expectedCode = code;
  options.sourceSha256 = source;
  options.keysetSha256 = keyset;
  options.context = context;
  options.continueWork = continueWork;
  const auto result =
      file.fileSize() > MAX_SOURCE_SIZE ? Result::TooLarge : validate({&file, readInput}, SCHEMA, metadata, options);
  file.close();
  if (result != Result::Ok) LOG_ERR("LANG", "Download validation: %s", resultName(result));
  return result;
}

Result I18n::install(const char* path, Installed& result) {
  HalFile source;
  if (!Storage.openFileForRead("LANG", path, source)) return Result::Io;
  if (source.fileSize() > MAX_SOURCE_SIZE) {
    source.close();
    LOG_ERR("LANG", "Language source too large");
    return Result::TooLarge;
  }
  CacheFile cache;
  if (!cache.open()) {
    source.close();
    return Result::StorageUnavailable;
  }
  // One 64 KiB staging slot (PSRAM on the device) turns per-string SD writes
  // into one write of the finished slot; the old slot stays untouched until then.
  HeapByteBuffer staging = allocateLarge(SLOT_SIZE);
  if (!staging) {
    LOG_ERR("LANG", "Cannot allocate %u bytes for the language install", unsigned(SLOT_SIZE));
    source.close();
    cache.close();
    return Result::Memory;
  }
  const uint64_t start = microseconds();
  const Flash flash{&cache, CACHE_SIZE, CacheFile::read, CacheFile::write, CacheFile::erase, CacheFile::verify};
  Result status =
      language_cache::install({&source, readInput, rewindInput}, SCHEMA, flash, slot_, result, staging.get());
  source.close();
  if (!cache.close() && status == Result::Ok) status = Result::Io;
  staging.reset();
  if (status == Result::Unchanged) {
    LOG_INF("LANG", "Language already active; cache unchanged");
  } else if (status != Result::Ok) {
    LOG_ERR("LANG", "Install failed: %s", resultName(status));
  } else {
    LOG_INF("LANG", "Installed %s in %llu us, slot=%d generation=%llu", result.metadata.code,
            static_cast<unsigned long long>(microseconds() - start), result.slot,
            static_cast<unsigned long long>(result.generation));
    if (!activateSlot(result.slot, result.generation)) status = Result::Io;
  }
  return status;
}
