#include "PersistableStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <ObfuscationUtils.h>

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

bool PersistableStoreBase::writeStringToFileAtomically(const char* path, const String& json) {
  // Callers may save unchanged values; an identical file is not rewritten.
  if (Storage.exists(path) && Storage.readFile(path) == json) return true;
  Storage.mkdir("/.crossdink");

  const std::string tempPath = std::string(path) + ".tmp";
  if (Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())) {
    LOG_ERR("PERSIST", "Failed to remove stale temporary file %s", tempPath.c_str());
    return false;
  }
  if (!Storage.writeFile(tempPath.c_str(), json)) {
    LOG_ERR("PERSIST", "Failed to write temporary file %s", tempPath.c_str());
    return false;
  }
  return replaceWithTemp(path);
}

bool PersistableStoreBase::replaceWithTemp(const char* path) {
  const std::string tempPath = std::string(path) + ".tmp";
  const std::string backupPath = std::string(path) + ".bak";
  const bool hadOriginal = Storage.exists(path);
  if (hadOriginal) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("PERSIST", "Failed to remove stale backup %s", backupPath.c_str());
      Storage.remove(tempPath.c_str());
      return false;
    }
    if (!Storage.rename(path, backupPath.c_str())) {
      LOG_ERR("PERSIST", "Failed to back up %s", path);
      Storage.remove(tempPath.c_str());
      return false;
    }
  }

  if (!Storage.rename(tempPath.c_str(), path)) {
    LOG_ERR("PERSIST", "Failed to replace %s", path);
    if (hadOriginal && !Storage.rename(backupPath.c_str(), path)) {
      LOG_ERR("PERSIST", "Failed to restore backup %s", backupPath.c_str());
    }
    Storage.remove(tempPath.c_str());
    return false;
  }

  if (hadOriginal && Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("PERSIST", "Failed to remove completed backup %s", backupPath.c_str());
  }
  return true;
}

bool PersistableStoreBase::readDocFromFile(const char* path, JsonDocument& doc) {
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
    LOG_ERR("PERSIST", "Failed to read %s (empty)", path);
    return false;
  }
  auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("PERSIST", "JSON parse error in %s: %s", path, error.c_str());
    return false;
  }
  return true;
}

bool PersistableStoreBase::recoverBackup(const char* path) {
  if (Storage.exists(path)) return true;
  const std::string backupPath = std::string(path) + ".bak";
  if (!Storage.exists(backupPath.c_str()) || !Storage.rename(backupPath.c_str(), path)) return false;
  LOG_INF("PERSIST", "Recovered interrupted write for %s", path);
  return true;
}
