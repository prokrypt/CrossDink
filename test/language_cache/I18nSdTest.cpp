// Round trip of the SD language backend in lib/I18n: discover a YAML file,
// install it into the two-slot cache file, re-open it at "boot", and fall back
// when the cache is missing or damaged. Runs against the host filesystem stubs.
#include <HalStorage.h>
#include <I18n.h>
#include <I18nStrings.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#define CHECK(...)                                                   \
  do {                                                               \
    if (!(__VA_ARGS__)) {                                            \
      std::fprintf(stderr, "line %d: %s\n", __LINE__, #__VA_ARGS__); \
      std::exit(1);                                                  \
    }                                                                \
  } while (0)

namespace {
std::string root;
void writeYaml(const char* name, const char* title) {
  std::filesystem::create_directories(root + language_cache::DIRECTORY);
  std::ofstream out(root + language_cache::DIRECTORY + "/" + name);
  out << "_language_code: \"XX\"\n_language_name: \"Test language\"\n_keyboard: \"DE\"\n";
  out << "STR_SETTINGS_TITLE: \"" << title << "\"\nSTR_UNKNOWN_KEY_FOR_TEST: \"ignored\"\n";
}
}  // namespace

int main() {
  char dir[] = "/tmp/crossdink-i18n-sd-XXXXXX";
  CHECK(mkdtemp(dir));
  root = dir;
  setenv("CROSSINK_TEST_SD", dir, 1);
  const std::string english = tr(STR_SETTINGS_TITLE);

  // No cache yet: a built-in code boots to its pack. The host stub inflates no
  // pack, so this is the reduced-language fallback: English, generation 0.
  I18N.begin("DE", 0);
  CHECK(std::strcmp(I18N.getCode(), "EN") == 0 && I18N.getGeneration() == 0);
  CHECK(english == tr(STR_SETTINGS_TITLE));

  I18n::Catalog catalog;
  CHECK(I18N.discover(catalog) == language_cache::Result::Ok);
  const size_t builtins = catalog.count;
  writeYaml("test.yaml", "Einstellungen");
  CHECK(I18N.discover(catalog) == language_cache::Result::Ok && catalog.count == builtins + 1);
  size_t row = 0;
  while (row < catalog.count && std::strcmp(catalog.code(row), "XX") != 0) ++row;
  CHECK(row < catalog.count && std::strcmp(catalog.name(row), "Test language") == 0);
  I18n::Option option;
  catalog.select(row, option);
  CHECK(option.path[0] && option.generation == 0);

  // Install: compiled into the cache file and active at once, keyboard follows _keyboard.
  language_cache::Installed installed;
  CHECK(I18N.install(option.path, installed) == language_cache::Result::Ok);
  CHECK(std::string("Einstellungen") == tr(STR_SETTINGS_TITLE));
  CHECK(std::strcmp(I18N.getCode(), "XX") == 0 && I18N.getLanguage() == Language::DE);
  CHECK(I18N.getGeneration() == installed.generation && installed.generation == 1);
  CHECK(std::filesystem::file_size(root + I18n::CACHE_FILE) == 2 * language_cache::SLOT_SIZE);
  CHECK(HalFile::liveFiles == 0);
  // The catalog now shows the active SD language as cached and keeps its file.
  CHECK(I18N.discover(catalog) == language_cache::Result::Ok);
  row = 0;
  while (std::strcmp(catalog.code(row), "XX") != 0) ++row;
  catalog.select(row, option);
  CHECK(option.generation == 1 && option.path[0]);
  // Same file again: nothing to do.
  CHECK(I18N.install(option.path, installed) == language_cache::Result::Unchanged);
  // Changed file: the other slot, next generation, live switch.
  writeYaml("test.yaml", "Optionen");
  CHECK(I18N.install(option.path, installed) == language_cache::Result::Ok);
  CHECK(installed.generation == 2 && std::string("Optionen") == tr(STR_SETTINGS_TITLE));

  // Boot: the saved code and generation select the newest slot; a stale
  // generation or a missing cache falls back to the built-in language.
  I18N.begin("XX", 2);
  CHECK(std::string("Optionen") == tr(STR_SETTINGS_TITLE) && I18N.getGeneration() == 2);
  I18N.begin("XX", 1);
  CHECK(std::string("Einstellungen") == tr(STR_SETTINGS_TITLE) && I18N.getGeneration() == 1);
  I18N.begin("XX", 7);
  CHECK(english == tr(STR_SETTINGS_TITLE) && I18N.getGeneration() == 0 && I18N.getLanguage() == Language::EN);
  I18N.begin("XX", 0);
  CHECK(std::string("Optionen") == tr(STR_SETTINGS_TITLE) && I18N.getGeneration() == 2);
  // Damaged slot (first record byte flipped): rejected by the CRC, English fallback, no crash.
  {
    const auto at =
        language_cache::slotOffset(2 * language_cache::SLOT_SIZE, installed.slot) + language_cache::HEADER_SIZE;
    std::fstream cache(root + I18n::CACHE_FILE, std::ios::in | std::ios::out | std::ios::binary);
    cache.seekg(at);
    const char before = static_cast<char>(cache.get());
    cache.seekp(at);
    cache.put(static_cast<char>(before ^ 0x55));
    CHECK(cache.good());
  }
  I18N.begin("XX", 2);
  CHECK(english == tr(STR_SETTINGS_TITLE) && I18N.getGeneration() == 0);
  CHECK(HalFile::liveFiles == 0);
  std::filesystem::remove_all(dir);
  std::puts("I18n SD language tests passed: discover, install, unchanged, update, boot, fallback");
  return 0;
}
