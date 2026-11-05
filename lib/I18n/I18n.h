#pragma once

#include <LanguageCache.h>
#include <Memory.h>

#include <cstdint>

#include "I18nKeys.h"
/**
 * Internationalization (i18n) system for CrossPoint Reader
 */

class I18n {
 public:
  static I18n& getInstance();

  // Disable copy
  I18n(const I18n&) = delete;
  I18n& operator=(const I18n&) = delete;

  // Get localized string by ID
  const char* get(StrId id) const;

  const char* operator[](StrId id) const { return get(id); }

  // Boot: activates the saved language. A language installed from an SD YAML
  // file (same code, and generation when given) beats the built-in pack.
  void begin(const char* code, uint64_t generation = 0);
  // Keyboard/RTL identity: the built-in language, or an SD language's _keyboard code.
  Language getLanguage() const { return _language; }
  const char* getCode() const { return code_; }
  const char* getName() const;
  uint64_t getGeneration() const { return generation_; }
  void setLanguage(Language lang);
  const char* getLanguageName(Language lang) const;
  static Language languageFromCode(const char* code);

  // Get all unique characters used in a specific language
  // Returns a sorted string of unique characters
  static const char* getCharacterSet(Language lang);

  struct Option {
    char code[language_cache::CODE_SIZE] = {};
    char name[language_cache::NAME_SIZE] = {};
    char path[160] = {};  // Empty for a built-in language or the active SD language.
    uint64_t generation = 0;
    bool duplicate = false;
  };
  // One fallible, <3 KiB catalog of built-in languages and SD YAML files.
  struct Catalog {
    static constexpr size_t MAX_SOURCE_FILES = 64;
    static constexpr size_t CAPACITY = getLanguageCount() + MAX_SOURCE_FILES + 1;
    static constexpr size_t TEXT_BYTES = 2048;
    struct Entry {
      uint16_t code, name, file;
      bool duplicate, cached;
    };
    Entry entries[CAPACITY];
    char text[TEXT_BYTES];
    size_t count = 0, used = 1;
    uint64_t cachedGeneration = 0;
    void reset();
    bool add(const char* code, const char* name, const char* filename, bool cached = false);
    bool source(size_t index, const char* name, const char* filename);
    const char* code(size_t i) const { return text + entries[i].code; }
    const char* name(size_t i) const { return text + entries[i].name; }
    const char* file(size_t i) const { return text + entries[i].file; }
    bool disabled(size_t i) const { return entries[i].duplicate; }
    void select(size_t index, Option& result) const;

   private:
    uint16_t append(const char* value);
  };
  // Settings-only: lists built-in languages plus the YAML files under
  // language_cache::DIRECTORY. Never boot or render work.
  language_cache::Result discover(Catalog& catalog) const;
  // Settings-only: compiles one SD YAML file into the cache file and activates
  // it. Unchanged means the active language already matches the file.
  language_cache::Result install(const char* path, language_cache::Installed& result);
  // Download verification only: the file must carry this firmware's English
  // source and key set hashes. No cache writes, settings writes or activation.
  language_cache::Result validateSource(const char* path, const char* code, const char* source, const char* keyset,
                                        void* context = nullptr, bool (*continueWork)(void*) = nullptr) const;
  static const char* sourceContract();
  static const char* keysetContract();
  // Every non-English language of the firmware's table, built in or not.
  static bool supportedDownload(const char* code);

  static constexpr const char* CACHE_FILE = "/.crossdink/languages/.cache.bin";

 private:
  I18n() : _language(Language::EN) {}
  bool activateSlot(int slot, uint64_t generation);
  void publishBuiltin(Language lang);

  Language _language;
  char code_[language_cache::CODE_SIZE] = "EN";
  char name_[language_cache::NAME_SIZE] = {};  // SD language display name; empty for built-ins.
  uint64_t generation_ = 0;
  int slot_ = -1;  // Cache slot of the active SD language, -1 for a built-in.
};

// Convenience macros
#define tr(id) I18n::getInstance().get(StrId::id)
#define I18N I18n::getInstance()
