#include "ThemePack.h"

#include <ArduinoJson.h>
#include <Bitmap.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Icon.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_attr.h>
#include <uzlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "ThemePackFields.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace ThemePack {
namespace {

constexpr const char* kRoots[] = {"/.themes", "/themes"};
constexpr size_t kMaxJsonBytes = 16 * 1024;
constexpr uint32_t kMaxCrashes = 2;
constexpr size_t kMaxListed = 64;  // picker indices are uint8_t

// Every metrics table theme code reads through themed(); inline, so one address per table.
constexpr const ThemeMetrics* kBuiltIns[] = {
    &BaseMetrics::values,    &LyraMetrics::values,      &Lyra3CoversMetrics::values, &LyraCarouselMetrics::values,
    &MinimalMetrics::values, &DashboardMetrics::values, &RoundedRaffMetrics::values,
};

struct BaseName {
  const char* name;
  CrossPointSettings::UI_THEME theme;
};
constexpr BaseName kBases[] = {
    {"classic", CrossPointSettings::CLASSIC},
    {"lyra", CrossPointSettings::LYRA},
    {"lyra-3-covers", CrossPointSettings::LYRA_3_COVERS},
    {"roundedraff", CrossPointSettings::ROUNDEDRAFF},
    {"lyra-carousel", CrossPointSettings::LYRA_CAROUSEL},
    {"minimal", CrossPointSettings::MINIMAL},
    {"dashboard", CrossPointSettings::DASHBOARD},
    {"cover-grid", CrossPointSettings::COVER_GRID},
};

// Indexed by UIIcon.
constexpr const char* kIconNames[] = {"folder",   "text",    "image", "book", "bookmark", "file",  "recent", "settings",
                                      "transfer", "library", "opds",  "wifi", "hotspot",  "chart", "flask"};
constexpr int kIconCount = static_cast<int>(std::size(kIconNames));
static_assert(kIconCount == UIIcon::Flask + 1, "one name per UIIcon");
constexpr int kIconBytes = 32 * 4;  // a 32 px icon: 32 rows of 4 bytes; 24 px uses 24 x 3

// One allocation per active pack (~6 KB, so it lands in PSRAM on the S3): the
// built-in tables with the overrides applied, and the decoded icons. Freed when
// the pack is turned off; nothing is allocated while no pack is set.
struct Pack {
  uint8_t base = 0;
  ThemeMetrics metrics[std::size(kBuiltIns)];
  freeink::Icon icons[kIconCount][2] = {};  // [24 px, 32 px]; bits == nullptr: built-in
  uint8_t iconBits[kIconCount][2][kIconBytes];
};

std::unique_ptr<Pack> gPack;
char gTried[sizeof(CrossPointSettings::uiThemePack)] = "";  // last name sync() read, loaded or not
const char* gToast = nullptr;

// Crash guard. RTC_NOINIT survives panics, watchdog resets and deep sleep; it is
// noise after power-on, and another firmware may have left data here, so any
// failed check starts from zero (CrossPointState's ReaderLoadGuard pattern).
struct CrashGuard {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t crashes;
  uint32_t crc;
};
RTC_NOINIT_ATTR CrashGuard crashGuard;
constexpr uint32_t GUARD_MAGIC = 0x43445450;  // "CDTP", CrossDink only
constexpr uint16_t GUARD_VERSION = 1;

uint32_t guardCrc() { return uzlib_crc32(&crashGuard, offsetof(CrashGuard, crc), 0); }

void sealGuard(const uint32_t crashes) {
  crashGuard = {GUARD_MAGIC, GUARD_VERSION, sizeof(CrashGuard), crashes, 0};
  crashGuard.crc = guardCrc();
}

bool guardValid() {
  return crashGuard.magic == GUARD_MAGIC && crashGuard.version == GUARD_VERSION &&
         crashGuard.size == sizeof(CrashGuard) && crashGuard.crc == guardCrc() && crashGuard.crashes < kMaxCrashes;
}

// A folder name, not a path.
bool validName(const char* name) {
  const size_t len = std::strlen(name);
  return len > 0 && len < sizeof(gTried) && name[0] != '.' && !std::strchr(name, '/') && !std::strchr(name, '\\');
}

// A file inside the pack folder.
bool validRelativePath(const char* path) {
  return path[0] != '\0' && path[0] != '/' && !std::strstr(path, "..") && !std::strchr(path, '\\');
}

bool findDir(const char* name, char* dir, const size_t size) {
  for (const char* root : kRoots) {
    std::snprintf(dir, size, "%s/%s", root, name);
    if (Storage.exists(dir)) return true;
  }
  return false;
}

bool readJson(const char* path, JsonDocument& doc) {
  HalFile file;
  if (!Storage.openFileForReadIfPresent("THM", path, file)) {
    LOG_ERR("THM", "%s: missing", path);
    return false;
  }
  const size_t size = file.size();
  if (size == 0 || size > kMaxJsonBytes) {
    file.close();
    LOG_ERR("THM", "%s: %u bytes (1 to %u allowed)", path, static_cast<unsigned>(size),
            static_cast<unsigned>(kMaxJsonBytes));
    return false;
  }
  auto text = makeUniqueNoThrow<char[]>(size);
  if (!text) {
    file.close();
    LOG_ERR("THM", "%s: out of memory", path);
    return false;
  }
  const bool read = file.read(text.get(), size) == static_cast<int>(size);
  file.close();
  if (!read) {
    LOG_ERR("THM", "%s: short read", path);
    return false;
  }
  const DeserializationError error = deserializeJson(doc, text.get(), size);
  if (error) {
    LOG_ERR("THM", "%s: %s", path, error.c_str());
    return false;
  }
  return true;
}

void loadIcon(Pack& pack, const int slotIcon, const char* dir, const char* file) {
  if (!validRelativePath(file)) {
    LOG_ERR("THM", "icon %s: \"%s\" must be a file in the pack folder", kIconNames[slotIcon], file);
    return;
  }
  char path[128];
  std::snprintf(path, sizeof(path), "%s/%s", dir, file);
  HalFile f;
  if (!Storage.openFileForReadIfPresent("THM", path, f)) {
    LOG_ERR("THM", "%s: missing", path);
    return;
  }
  Bitmap bmp(f);
  bmp.setBwOutput(128);
  const BmpReaderError err = bmp.parseHeaders();
  const int w = bmp.getWidth();
  const int h = bmp.getHeight();
  const char* problem = err != BmpReaderError::Ok        ? Bitmap::errorToString(err)
                        : !bmp.is1Bit()                  ? "not a 1-bit BMP"
                        : w != h || (w != 24 && w != 32) ? "not 24x24 or 32x32"
                        : bmp.getRowBytes() > 4          ? "bad row size"
                                                         : nullptr;
  if (problem) {
    f.close();
    LOG_ERR("THM", "%s: %s", path, problem);
    return;
  }
  const int slot = w == 32 ? 1 : 0;
  const int stride = (w + 7) / 8;
  uint8_t* bits = pack.iconBits[slotIcon][slot];
  std::memset(bits, 0xFF, static_cast<size_t>(stride) * h);  // Mask1: 1 = transparent
  uint8_t rowBuffer[4] = {};
  uint8_t out[8] = {};  // 32 px at 2 bits
  long inkRows = 0;
  int inkPixels = 0;
  for (int row = 0; row < h; ++row) {
    if (bmp.readNextRow(out, rowBuffer) != BmpReaderError::Ok) {
      f.close();
      LOG_ERR("THM", "%s: short read", path);
      return;
    }
    const int y = bmp.isTopDown() ? row : h - 1 - row;
    for (int x = 0; x < w; ++x) {
      if (((out[x / 4] >> (6 - 2 * (x % 4))) & 0x03) >= 2) continue;  // white
      bits[y * stride + x / 8] &= static_cast<uint8_t>(~(0x80 >> (x % 8)));
      inkRows += y;
      ++inkPixels;
    }
  }
  f.close();
  const auto centerY = static_cast<int16_t>(inkPixels > 0 ? inkRows / inkPixels : h / 2);
  pack.icons[slotIcon][slot] = {static_cast<uint16_t>(w), static_cast<uint16_t>(h), centerY, bits};
}

std::unique_ptr<Pack> load(const char* name) {
  if (!validName(name)) {
    LOG_ERR("THM", "\"%s\": not a folder name", name);
    return nullptr;
  }
  char dir[64];
  if (!findDir(name, dir, sizeof(dir))) {
    LOG_ERR("THM", "%s: not in /.themes or /themes", name);
    return nullptr;
  }
  char path[96];
  std::snprintf(path, sizeof(path), "%s/theme.json", dir);
  JsonDocument doc;
  if (!readJson(path, doc)) return nullptr;

  const char* baseName = doc["base"] | "";
  const BaseName* base = std::find_if(std::begin(kBases), std::end(kBases),
                                      [baseName](const BaseName& b) { return std::strcmp(b.name, baseName) == 0; });
  if (base == std::end(kBases)) {
    LOG_ERR("THM", "%s: unknown base \"%s\"", path, baseName);
    return nullptr;
  }
  // Touch boards drop these two from the theme picker (SettingsList.h); a pack cannot bring them back.
  if (gpio.hasTouch() &&
      (base->theme == CrossPointSettings::CLASSIC || base->theme == CrossPointSettings::ROUNDEDRAFF)) {
    LOG_ERR("THM", "%s: base \"%s\" is not offered on touch devices", path, baseName);
    return nullptr;
  }

  auto pack = makeUniqueNoThrow<Pack>();
  if (!pack) {
    LOG_ERR("THM", "%s: out of memory", name);
    return nullptr;
  }
  pack->base = static_cast<uint8_t>(base->theme);
  for (size_t i = 0; i < std::size(kBuiltIns); ++i) pack->metrics[i] = *kBuiltIns[i];

  int overrides = 0;
  for (const JsonPairConst kv : doc["metrics"].as<JsonObjectConst>()) {
    const char* key = kv.key().c_str();
    const int field = ThemePackFields::find(key);
    if (field < 0) {
      LOG_ERR("THM", "%s: metric %s unknown, ignored", path, key);
      continue;
    }
    int32_t value = 0;
    const auto parsed = ThemePackFields::parse(field, kv.value(), value);
    if (parsed == ThemePackFields::Parse::Bad) {
      LOG_ERR("THM", "%s: metric %s has the wrong type, ignored", path, key);
      continue;
    }
    if (parsed == ThemePackFields::Parse::Clamped) {
      LOG_ERR("THM", "%s: metric %s clamped to %ld", path, key, static_cast<long>(value));
    }
    for (auto& metrics : pack->metrics) ThemePackFields::apply(metrics, field, value);
    ++overrides;
  }

  int icons = 0;
  for (const JsonPairConst kv : doc["icons"].as<JsonObjectConst>()) {
    const char* key = kv.key().c_str();
    const auto it = std::find_if(std::begin(kIconNames), std::end(kIconNames),
                                 [key](const char* n) { return std::strcmp(n, key) == 0; });
    if (it == std::end(kIconNames)) {
      LOG_ERR("THM", "%s: icon %s unknown, ignored", path, key);
      continue;
    }
    const int slotIcon = static_cast<int>(it - std::begin(kIconNames));
    // One file, or a list with a 24 px and a 32 px file.
    if (kv.value().is<const char*>()) {
      loadIcon(*pack, slotIcon, dir, kv.value().as<const char*>());
    } else {
      for (const JsonVariantConst file : kv.value().as<JsonArrayConst>()) loadIcon(*pack, slotIcon, dir, file | "");
    }
    icons += (pack->icons[slotIcon][0].bits != nullptr) + (pack->icons[slotIcon][1].bits != nullptr);
  }

  for (const JsonPairConst kv : doc.as<JsonObjectConst>()) {
    const char* key = kv.key().c_str();
    if (std::strcmp(key, "name") && std::strcmp(key, "base") && std::strcmp(key, "metrics") &&
        std::strcmp(key, "icons")) {
      LOG_ERR("THM", "%s: key %s not supported, ignored", path, key);
    }
  }
  LOG_INF("THM", "loaded %s: base %s, %d metrics, %d icons", name, base->name, overrides, icons);
  return pack;
}

}  // namespace

void sync(const bool force) {
  const char* want = SETTINGS.uiThemePack;
  if (want[0] == '\0') {
    gPack.reset();
    gTried[0] = '\0';
    return;
  }
  if (force || std::strcmp(want, gTried) != 0) {
    std::snprintf(gTried, sizeof(gTried), "%s", want);
    gPack.reset();
    gPack = load(want);
    if (!gPack) gToast = tr(STR_THEME_PACK_ERROR);
  }
  if (gPack) SETTINGS.uiTheme = gPack->base;
}

bool active() { return gPack != nullptr; }

const freeink::Icon* icon(const UIIcon which, const uint32_t size) {
  const Pack* pack = gPack.get();
  const int index = static_cast<int>(which);
  if (!pack || index < 0 || index >= kIconCount || (size != 24 && size != 32)) return nullptr;
  const freeink::Icon& i = pack->icons[index][size == 32 ? 1 : 0];
  return i.bits ? &i : nullptr;
}

std::vector<std::string> list() {
  std::vector<std::string> names;
  char name[sizeof(gTried) + 1];
  for (const char* root : kRoots) {
    HalFile dir = Storage.open(root);
    if (!dir || !dir.isDirectory()) continue;
    while (true) {
      HalFile entry = dir.openNextFile();
      if (!entry) break;
      const bool isDir = entry.isDirectory();
      name[0] = '\0';
      if (isDir) entry.getName(name, sizeof(name));
      entry.close();
      if (isDir && validName(name) && std::find(names.begin(), names.end(), name) == names.end()) {
        names.emplace_back(name);
      }
      if (names.size() >= kMaxListed) break;
    }
    dir.close();
  }
  std::sort(names.begin(), names.end());
  return names;
}

void bootGuard(const esp_reset_reason_t reason) {
  const bool crash = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                     reason == ESP_RST_WDT || reason == ESP_RST_CPU_LOCKUP;
  uint32_t crashes = guardValid() && crash && SETTINGS.uiThemePack[0] != '\0' ? crashGuard.crashes + 1 : 0;
  if (crashes >= kMaxCrashes) {
    LOG_ERR("THM", "%lu crash reboots in a row with pack %s: pack turned off", static_cast<unsigned long>(crashes),
            SETTINGS.uiThemePack);
    SETTINGS.uiThemePack[0] = '\0';
    SETTINGS.saveToFile();  // rare recovery path, so not deferred to sleep
    gToast = tr(STR_THEME_PACK_CRASHED);
    crashes = 0;
  }
  sealGuard(crashes);
}

const char* takeToast() { return std::exchange(gToast, nullptr); }

}  // namespace ThemePack

const ThemeMetrics& themed(const ThemeMetrics& builtIn) {
  const auto* pack = ThemePack::gPack.get();
  if (!pack) return builtIn;
  const auto* end = std::end(ThemePack::kBuiltIns);
  const auto* it = std::find(std::begin(ThemePack::kBuiltIns), end, &builtIn);
  return it == end ? builtIn : pack->metrics[it - std::begin(ThemePack::kBuiltIns)];
}
