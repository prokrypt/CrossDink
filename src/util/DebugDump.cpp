#include "DebugDump.h"

#include <HalStorage.h>
#include <SdCardFontRegistry.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "SettingsList.h"

namespace DebugDump {
namespace {

void append(std::string& out, const char* fmt, const unsigned a) {
  char item[64];
  const int n = snprintf(item, sizeof(item), fmt, a);
  if (n > 0) out.append(item, std::min<size_t>(static_cast<size_t>(n), sizeof(item) - 1));
}

// Family folder names under one font root (as the registry's discover() reads them).
void appendFonts(std::string& out, const char* root) {
  HalFile dir = Storage.open(root);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  out += "=== fonts ";
  out += root;
  out += ':';
  char name[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    const bool isDirectory = entry.isDirectory();
    if (isDirectory) entry.getName(name, sizeof(name));
    entry.close();
    if (!isDirectory || name[0] == '.' || name[0] == '_') continue;
    out += ' ';
    out += name;
  }
  dir.close();
  out += '\n';
}

}  // namespace

std::string format() {
  std::string out;
  out.reserve(3072);  // once per dump; PSRAM on device (above the 1 KB internal cutoff)
  out += "=== reader font: ";
  out += SETTINGS.sdFontFamilyName[0] ? SETTINGS.sdFontFamilyName
                                      : (SETTINGS.fontFamily == CrossPointSettings::BITTER ? "Bitter" : "Lexend Deca");
  append(out, " %u pt\n", SETTINGS.readerFontPointSize);
  out += "=== settings:";
  for (const SettingInfo& s : getBaseSettingsList()) {
    if (!s.key || !(s.type == SettingType::TOGGLE || s.type == SettingType::ENUM || s.type == SettingType::VALUE))
      continue;
    if (s.valuePtr) {
      out += ' ';
      out += s.key;
      append(out, "=%u", SETTINGS.*(s.valuePtr));
    } else if (s.value16Ptr) {
      out += ' ';
      out += s.key;
      append(out, "=%u", SETTINGS.*(s.value16Ptr));
    }
  }
  out += '\n';
  appendFonts(out, SdCardFontRegistry::FONTS_DIR_HIDDEN);
  appendFonts(out, SdCardFontRegistry::FONTS_DIR_VISIBLE);
  return out;
}

}  // namespace DebugDump
