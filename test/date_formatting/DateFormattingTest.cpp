// Month names in formatted dates come from the active language: built-in English, or an SD YAML
// language installed through the host HalStorage stubs (the host inflates no built-in packs).
#include <DateFormatting.h>
#include <HalStorage.h>
#include <I18n.h>

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

int main() {
  char date[32];
  CHECK(formatDateParts(date, sizeof(date), 2026, 10, 3, 0));
  CHECK(std::strcmp(date, "Oct 03, 2026") == 0);
  CHECK(formatDateParts(date, sizeof(date), 2026, 2, 3, 7));
  CHECK(std::strcmp(date, "February 03") == 0);
  CHECK(formatDateParts(date, sizeof(date), 2026, 2, 3, 4, '-'));
  CHECK(std::strcmp(date, "2026-02-03") == 0);
  CHECK(formatDateParts(date, sizeof(date), 2026, 2, 3, 3, '.'));
  CHECK(std::strcmp(date, "03.02.2026") == 0);
  CHECK(!formatDateParts(date, 4, 2026, 2, 3, 7));
  CHECK(!formatDateParts(date, sizeof(date), 2026, 0, 3, 0));
  CHECK(!formatDateParts(date, sizeof(date), 2026, 13, 3, 0));
  // The longest English month in every format fits the UI's date buffer.
  for (unsigned format = 0; format <= 8; ++format) CHECK(formatDateParts(date, sizeof(date), 2026, 9, 28, format));

  char dir[] = "/tmp/crossdink-date-XXXXXX";
  CHECK(mkdtemp(dir));
  const std::string root = dir;
  setenv("CROSSINK_TEST_SD", dir, 1);
  std::filesystem::create_directories(root + language_cache::DIRECTORY);
  {
    std::ofstream out(root + language_cache::DIRECTORY + "/de.yaml");
    out << "_language_code: \"XX\"\n_language_name: \"Test\"\n_keyboard: \"DE\"\n";
    out << "STR_MONTH_OCT_SHORT: \"Okt.\"\nSTR_MONTH_OCT: \"Oktober\"\n";
  }
  I18n::Catalog catalog;
  CHECK(I18N.discover(catalog) == language_cache::Result::Ok);
  size_t row = 0;
  while (row < catalog.count && std::strcmp(catalog.code(row), "XX") != 0) ++row;
  CHECK(row < catalog.count);
  I18n::Option option;
  catalog.select(row, option);
  language_cache::Installed installed;
  CHECK(I18N.install(option.path, installed) == language_cache::Result::Ok);
  CHECK(formatDateParts(date, sizeof(date), 2026, 10, 3, 0));
  CHECK(std::strcmp(date, "Okt. 03, 2026") == 0);
  CHECK(formatDateParts(date, sizeof(date), 2026, 10, 3, 8));
  CHECK(std::strcmp(date, "03 Oktober") == 0);
  std::filesystem::remove_all(root);
  return 0;
}
