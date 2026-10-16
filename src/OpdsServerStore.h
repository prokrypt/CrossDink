#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>
#include <vector>

enum class OpdsFilenameFormat : uint8_t {
  AUTHOR_TITLE = 0,
  TITLE_AUTHOR = 1,
};

const char* opdsFilenameFormatToJson(OpdsFilenameFormat format);
OpdsFilenameFormat opdsFilenameFormatFromJson(const char* value);

struct OpdsServer {
  std::string name;
  std::string url;
  std::string username;
  std::string password;  // Plaintext in memory; obfuscated with hardware key on disk
  OpdsFilenameFormat filenameFormat = OpdsFilenameFormat::AUTHOR_TITLE;
};

/**
 * Singleton class for storing OPDS server configurations on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON.
 */
class OpdsServerStore : public PersistableStore<OpdsServerStore> {
 private:
  std::vector<OpdsServer> servers;
  bool loaded_ = false;
  // Server list emptiness seen at the last release(): Home asks after every reader visit, so skip re-parsing
  // opds.json for it. shortcut: an opds.json replaced by file transfer or USB while released keeps the old answer
  // until the store loads again (OPDS settings, network boot); invalidate on those writes if it matters.
  int8_t hasServersHint_ = -1;

  OpdsServerStore() = default;
  bool migrateFromSettings();

  friend class PersistableStore<OpdsServerStore>;

 public:
  static constexpr size_t MAX_SERVERS = 8;
  static const char* getFilePath() { return "/.crossdink/opds.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();
  void ensureLoaded() const;
  void release();

  bool addServer(const OpdsServer& server);
  bool updateServer(size_t index, const OpdsServer& server);
  bool removeServer(size_t index);

  const std::vector<OpdsServer>& getServers() const {
    ensureLoaded();
    return servers;
  }
  const OpdsServer* getServer(size_t index) const;
  size_t getCount() const {
    ensureLoaded();
    return servers.size();
  }
  bool hasServers() const {
    if (!loaded_ && hasServersHint_ >= 0) return hasServersHint_ != 0;
    ensureLoaded();
    return !servers.empty();
  }
};

#define OPDS_STORE OpdsServerStore::getInstance()
