#include "PersistableStore.h"

#include <HalStorage.h>
#include <InPlaceFileWrite.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

#include <cstring>

bool PersistableStoreBase::writeDocToFile(const char* path, const JsonDocument& doc) {
  Storage.mkdir("/.crossdink");
  String json;
  serializeJson(doc, json);
  if (!Storage.writeFile(path, json)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::writeDocToFileAtomically(const char* path, const JsonDocument& doc) {
  String json;
  serializeJson(doc, json);
  return writeStringToFileAtomically(path, json);
}

void PersistableStoreBase::serializeDoc(const JsonDocument& doc, String& out) {
  out = String();
  serializeJson(doc, out);
}

bool PersistableStoreBase::writeStringToFileAtomically(const char* path, const String& json, const bool compare) {
  // Two copies rewritten in place, .bak first, so one always parses (readDocFromFile
  // falls back to .bak). No temp file, rename or remove: creating recent.json.tmp
  // took ~139 ms on the card. Padding spaces from a longer save are ignored.
  // Callers may save unchanged values; an identical file is not rewritten.
  if (compare && Storage.exists(path)) {
    const String onCard = Storage.readFile(path);
    size_t onCardLen = onCard.length();
    while (onCardLen > 0 && onCard[onCardLen - 1] == ' ') --onCardLen;
    if (onCardLen == json.length() && strncmp(onCard.c_str(), json.c_str(), onCardLen) == 0) return true;
  }
  Storage.mkdir("/.crossdink");
  const std::string backupPath = std::string(path) + ".bak";
  const auto* data = reinterpret_cast<const uint8_t*>(json.c_str());
  if (!writeFileInPlace("PERSIST", backupPath.c_str(), data, json.length(), true) ||
      !writeFileInPlace("PERSIST", path, data, json.length(), true)) {
    LOG_ERR("PERSIST", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
  String heldCopy;
  if (heldJson && heldJson(path, heldCopy)) {
    const auto error = deserializeJson(doc, heldCopy);
    if (!error) return true;
    LOG_ERR("PERSIST", "JSON parse error in held %s: %s; reading the card", path, error.c_str());
    doc.clear();
  }
  if (cardJson) cardJson(path, nullptr, 0);  // reading the card below: forget what it held until path parses
  std::string recoveryPath;
  const char* readPath = path;
  if (!Storage.exists(path)) {
    recoveryPath = std::string(path) + ".bak";
    if (!Storage.exists(recoveryPath.c_str())) {
      return false;  // Expected on first boot — not an error.
    }
    if (Storage.rename(recoveryPath.c_str(), path)) {
      LOG_INF("PERSIST", "Recovered interrupted write for %s", path);
    } else {
      LOG_ERR("PERSIST", "Could not restore backup for %s; reading backup directly", path);
      readPath = recoveryPath.c_str();
    }
  }
  String json = Storage.readFile(readPath);
  if (json.isEmpty()) {
    LOG_ERR("PERSIST", "Failed to read %s (empty)", readPath);
  } else {
    const auto error = deserializeJson(doc, json);
    if (!error) {
      if (cardJson && readPath == path) cardJson(path, json.c_str(), json.length());
      return true;
    }
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", readPath, error.c_str());
    doc.clear();
  }
  // A save torn mid-write leaves path unreadable; its .bak copy was written first.
  if (readPath != path) return false;
  recoveryPath = std::string(path) + ".bak";
  if (!Storage.exists(recoveryPath.c_str())) return false;
  json = Storage.readFile(recoveryPath.c_str());
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", recoveryPath.c_str(), error.c_str());
    return false;
  }
  LOG_ERR("PERSIST", "%s unreadable; loaded %s", path, recoveryPath.c_str());
  return true;
}

bool PersistableStoreBase::recoverBackup(const char* path) {
  if (Storage.exists(path)) return true;
  const std::string backupPath = std::string(path) + ".bak";
  if (!Storage.exists(backupPath.c_str()) || !Storage.rename(backupPath.c_str(), path)) return false;
  LOG_INF("PERSIST", "Recovered interrupted write for %s", path);
  return true;
}
