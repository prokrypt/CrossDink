#include <AssetCompanionStore.h>
#include <AssetDownloadService.h>
#include <DownloadFileSwap.h>
#include <HttpDownloader.h>
#include <I18n.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <cassert>
#include <cstring>
#include <iostream>

using namespace AssetDownloadService;
std::string response;
std::function<void()> beforeValidation;
int downloads = 0;
bool cancelledTransfer = false;
int companionPreparations = 0;
Result companionResult = Result::Ok;
Result AssetCompanionStore::prepare(const AssetCatalog::Catalog&, const AssetCatalog::Entry&, const Endpoints&,
                                    const Callbacks&) {
  ++companionPreparations;
  assert(downloads == 0 || companionResult == Result::Ok);
  return companionResult;
}
HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destination,
                                                             ProgressCallback progress, bool*, const std::string&,
                                                             const std::string&, DownloadOptions options) {
  ++downloads;
  assert(companionPreparations > 0 && companionResult == Result::Ok);
  assert(options.transport == Transport::ESP_HTTP);
  assert(options.stageAsPart && !options.preservePartial && !options.resumePartial);
  assert(options.checkFreeSpace && options.requireBackupCleanup);
  assert(options.bufferSize == 1024 && options.maxBytes == 262144 && options.maxRedirects == 2);
  assert(options.allowUrl(url, options.urlContext));
  if (options.shouldCancel && options.shouldCancel()) return ABORTED;
  if (response.size() != options.expectedBytes || response.size() > options.maxBytes) return HTTP_ERROR;
  const auto partial = destination + ".part";
  files[partial] = response;
  if (progress) progress(response.size(), response.size());
  if (cancelledTransfer) {
    files.erase(partial);
    return ABORTED;
  }
  if (beforeValidation) beforeValidation();
  if (!options.validateWithContext(partial, options.validationContext)) {
    files.erase(partial);
    return FILE_ERROR;
  }
  return DownloadFileSwap::publish(destination, options.requireBackupCleanup) ? OK : FILE_ERROR;
}
std::string digest(const std::string& bytes) {
  unsigned char result[32];
  SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), result);
  char out[65];
  for (size_t i = 0; i < 32; ++i) std::sprintf(out + 2 * i, "%02x", result[i]);
  return out;
}
const std::string yaml = "_language_code: \"DE\"\n_language_name: \"Deutsch\"\nSTR_TEST: \"Hallo\"\n";
void reset() {
  assetLogs.clear();
  files.clear();
  directories.clear();
  openReaders.clear();
  failRemove.clear();
  failRename.clear();
  failClose = failRead = failIteration = false;
  sourceValid = true;
  downloads = 0;
  response = yaml;
  beforeValidation = {};
  cancelledTransfer = false;
  companionPreparations = 0;
  companionResult = Result::Ok;
}
void put(const std::string& path, const std::string& bytes) {
  directories.insert(path.substr(0, path.find_last_of('/')));
  files[path] = bytes;
}
AssetCatalog::Catalog catalog(AssetCatalog::Kind kind = AssetCatalog::Kind::Translation) {
  AssetCatalog::Catalog result{};
  result.kind = kind;
  std::memset(result.revision, 'a', 40);
  result.revision[40] = 0;
  auto& entry = result.entries[0];
  std::strcpy(entry.code, kind == AssetCatalog::Kind::Translation ? "DE" : "de");
  entry.bytes = yaml.size();
  std::strcpy(entry.sha256, digest(yaml).c_str());
  result.count = 1;
  return result;
}
std::string namedYaml(const std::string& code, const std::string& name) {
  return "_language_code: \"" + code + "\"\n_language_name: \"" + name + "\"\nSTR_TEST: \"Hi\"\n";
}
void pinResponse(AssetCatalog::Entry& entry, const std::string& bytes) {
  response = bytes;
  entry.bytes = bytes.size();
  std::strcpy(entry.sha256, digest(bytes).c_str());
}
void cleanupRetryTests(const Endpoints& endpoints) {
  // Include a noncanonical approved filename so recovery follows the selected
  // source, not a freshly constructed canonical destination.
  for (const char* filename : {"DE.yaml", "mine.yaml"}) {
    reset();
    const auto cat = catalog();
    const auto& entry = cat.entries[0];
    const std::string destination = std::string("/.crossdink/languages/") + filename;
    const auto backup = destination + ".old";
    const auto old = yaml + "# old\n";
    put(destination, old);
    failRemove.insert(backup);
    assert(download(cat, entry, endpoints, true) == Result::Io && downloads == 1);
    assert(files.at(destination) == yaml && files.at(backup) == old);

    Source source;
    const auto publishedFiles = files;
    assert(discover(cat.kind, entry, source) == Result::Ok);
    assert(source.status == Status::Current && source.recoveryPending);
    assert(source.path == destination && source.sha256 == digest(yaml));
    assert(files == publishedFiles);  // discovery must not attempt cleanup
    assert(download(cat, entry, endpoints, false) == Result::Io && downloads == 1);
    assert(files == publishedFiles);  // persistent fault retains both copies
    assert(discover(cat.kind, entry, source) == Result::Ok && source.recoveryPending);

    failRemove.clear();
    assert(download(cat, entry, endpoints, false) == Result::Ok && downloads == 2);
    assert(files.at(destination) == yaml && !files.count(backup) && !files.count(destination + ".part"));
    assert(discover(cat.kind, entry, source) == Result::Ok);
    assert(source.status == Status::Current && !source.recoveryPending);
    for (const auto& reader : openReaders) assert(reader.second == 0);
  }
}
void companionGateTests(const Endpoints& endpoints) {
  auto cat = catalog();
  const auto& entry = cat.entries[0];
  const std::string destination = "/.crossdink/languages/DE.yaml";
  for (const auto failure : {Result::Cancelled, Result::Io, Result::Invalid, Result::Memory, Result::Network}) {
    reset();
    put(destination, yaml + "# previous source\n");
    companionResult = failure;
    assert(download(cat, entry, endpoints, true) == failure);
    assert(companionPreparations == 1 && downloads == 0);
    assert(files.at(destination) == yaml + "# previous source\n");
    assert(!files.count(destination + ".part") && !files.count(destination + ".old"));
  }
  reset();
  put(destination, yaml);
  assert(download(cat, entry, endpoints, false) == Result::Ok);
  assert(companionPreparations == 1 && downloads == 1 && files.at(destination) == yaml);
}

void sourceDiagnosticsTests() {
  const auto logged = [](std::string_view expected) {
    for (const auto& line : assetLogs)
      if (line.find(expected) != std::string::npos) return true;
    return false;
  };
  auto cat = catalog();
  auto& entry = cat.entries[0];
  std::strcpy(entry.code, "FR");
  Source source;
  reset();
  // Published-style Vietnamese metadata must not block downloading French.
  const std::string vietnamese =
      "_firmware_version: \"1.6.1-local\"\n_template_source_commit: \"61cc10a49ddffbcef3e9714038076d6d999e5785\"\n"
      "_template_source_sha256: \"39d64221d9bcb1a4d05a08dd12ea19ac8ef579b1e83c001e5c58537d15787322\"\n"
      "_template_keyset_sha256: \"29477e8b762a813886821526dc8c4146dbc112c09c1084688b43c68c87b40944\"\n" +
      namedYaml("VI", "Tiếng Việt");
  put("/.crossdink/languages/VI.yaml", vietnamese);
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Missing);
  assert(logged("SD metadata accepted: path=/.crossdink/languages/VI.yaml code=VI selected=FR"));
  assert(logged("SD check complete: code=FR"));
  for (const auto& broken :
       {std::string("_language_code: \"VI\"\n_language_name: unquoted\n"), std::string("\xef\xbb\xbf") + vietnamese}) {
    reset();
    put("/.crossdink/languages/VI.yaml", broken);
    const auto before = files;
    assert(discover(cat.kind, entry, source) == Result::Invalid && files == before && downloads == 0);
    assert(logged("SD metadata rejected: path=/.crossdink/languages/VI.yaml"));
    assert(logged("SD metadata details: selected=FR"));
    assert(logged(broken[0] == '_' ? "line=2" : "line=1"));
    assert(logged(broken[0] == '_' ? "invalid YAML syntax" : "UTF-8 BOM is not supported"));
  }
  reset();
  put("/.crossdink/languages/VI.yaml", vietnamese);
  failRead = true;
  assert(discover(cat.kind, entry, source) == Result::Io);
  assert(logged("reason=source read failed"));
  reset();
  // Long supported filenames must not push the rejection reason out of the
  // firmware's bounded log entry (the filename itself is logged separately).
  const auto longPath = "/.crossdink/languages/" + std::string(120, 'v') + ".yaml";
  put(longPath, "_language_code: \"VI\"\n_language_name: unquoted\n");
  assert(discover(cat.kind, entry, source) == Result::Invalid);
  assert(logged("SD metadata rejected: path=" + longPath));
  assert(logged("reason=invalid YAML syntax: expected key: \"value\" line=2 offset=46"));
  reset();
  const auto rejectedName = std::string(154, 'v') + ".yaml";
  put("/.crossdink/languages/" + rejectedName, vietnamese);
  assert(discover(cat.kind, entry, source) == Result::Invalid);
  assert(logged("SD filename rejected: name=" + rejectedName));
  assert(logged("SD filename reason=filename too long length=159"));
  reset();
}

void macMetadataTests() {
  const Endpoints endpoints{"https://assets.example.invalid/base/", ""};
  const std::string appleDouble = std::string("\x00\x05\x16\x07", 4) + std::string(4092, '\0');
  auto cat = catalog();
  auto& entry = cat.entries[0];
  std::strcpy(entry.code, "FR");
  const auto french = namedYaml("FR", "Français");
  Source source;
  reset();
  put("/.crossdink/languages/VI.yaml", namedYaml("VI", "Tiếng Việt"));
  put("/.crossdink/languages/._VI.yaml", appleDouble);
  put("/.crossdink/languages/._FR.yaml.old", appleDouble);
  const auto before = files;
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Missing);
  assert(files == before && downloads == 0);
  pinResponse(entry, french);
  assert(download(cat, entry, endpoints, false) == Result::Ok && downloads == 1);
  assert(files.at("/.crossdink/languages/FR.yaml") == french && files.size() == before.size() + 1);
  for (const auto& file : before) assert(files.at(file.first) == file.second);

  // An entire SD source catalog with Finder companions must retain capacity
  // for one more real language: sidecars are excluded from the source count.
  reset();
  for (size_t i = 0; i + 1 < I18n::Catalog::MAX_SOURCE_FILES; ++i) {
    const auto code = "X" + std::to_string(i);
    put("/.crossdink/languages/" + code + ".yaml", namedYaml(code, "Test"));
    put("/.crossdink/languages/._" + code + ".yaml", appleDouble);
  }
  pinResponse(entry, french);
  assert(download(cat, entry, endpoints, false) == Result::Ok && downloads == 1);
  assert(files.at("/.crossdink/languages/FR.yaml") == french);
  reset();
  for (size_t i = 0; i < I18n::Catalog::MAX_SOURCE_FILES; ++i) {
    const auto code = "X" + std::to_string(i);
    put("/.crossdink/languages/" + code + ".yaml", namedYaml(code, "Test"));
  }
  assert(download(cat, entry, endpoints, false) == Result::Invalid && downloads == 0);  // one over the limit
  reset();
}

int main() {
  macMetadataTests();
  sourceDiagnosticsTests();
  assert(digest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  Endpoints endpoints{"https://assets.example.invalid/base/", "https://cdn.example.invalid/approved/"};
  assert(allowUrl("https://assets.example.invalid/base/snapshots/file.yaml", &endpoints));
  assert(allowUrl("https://cdn.example.invalid/approved/file.yaml", &endpoints));
  for (const char* bad :
       {"http://assets.example.invalid/base/file", "https://assets.example.invalid.evil/base/file",
        "https://u@assets.example.invalid/base/file", "https://assets.example.invalid/base/../file",
        "https://assets.example.invalid/base/%2e%2e/file", "https://assets.example.invalid/base/file?q=1",
        "https://assets.example.invalid/base//file", "https://assets.example.invalid/base/./file",
        "https://assets.example.invalid/base/..", "https://assets.example.invalid/base/.",
        "https://assets.example.invalid/base/file#frag", "https://cdn.example.invalid/else/file"})
    assert(!allowUrl(bad, &endpoints));
  Endpoints empty;
  assert(!allowUrl("https://assets.example.invalid/base/file", &empty));
  auto cat = catalog();
  auto& entry = cat.entries[0];
  Source source;
  reset();
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Missing);
  assert(!source.recoveryPending);
  assert(download(cat, entry, empty, false) == Result::Unavailable && downloads == 0);
  struct Phases {
    bool verified = false;
    bool payloadStarted = false;
  } phases;
  Callbacks phaseCallbacks;
  phaseCallbacks.context = &phases;
  phaseCallbacks.verifying = [](void* context) { static_cast<Phases*>(context)->verified = true; };
  // Payload stage starts only after companions are prepared and before its transfer.
  phaseCallbacks.payloadStarted = [](void* context) {
    assert(companionPreparations > 0 && downloads == 0);
    static_cast<Phases*>(context)->payloadStarted = true;
  };
  assert(download(cat, entry, endpoints, false, phaseCallbacks) == Result::Ok && phases.verified);
  assert(phases.payloadStarted);
  assert(files.at("/.crossdink/languages/DE.yaml") == yaml && !files.count("/.crossdink/languages/DE.yaml.old"));
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Current);
  assert(!source.recoveryPending);
  assert(source.sha256 == digest(yaml));
  reset();
  put("/.crossdink/languages/mine.yaml", yaml);
  assert(discover(cat.kind, entry, source) == Result::Ok &&
         std::string(source.path) == "/.crossdink/languages/mine.yaml");
  files["/.crossdink/languages/mine.yaml"] += "# edited\n";
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Different);
  phases.payloadStarted = false;
  assert(download(cat, entry, endpoints, false, phaseCallbacks) == Result::OverwriteRequired && downloads == 0);
  assert(!phases.payloadStarted);
  assert(download(cat, entry, endpoints, true) == Result::Ok && !files.count("/.crossdink/languages/DE.yaml"));
  assert(files.at("/.crossdink/languages/mine.yaml") == yaml);
  reset();
  put("/.crossdink/languages/DE.yaml", yaml + "# old\n");
  std::strcpy(entry.previous, digest(files.at("/.crossdink/languages/DE.yaml")).c_str());
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Previous);
  assert(download(cat, entry, endpoints, false) == Result::Ok);
  reset();
  put("/.crossdink/languages/DE.yaml", yaml);
  put("/.crossdink/languages/copy.yaml", yaml);
  assert(discover(cat.kind, entry, source) == Result::Collision);
  assert(download(cat, entry, endpoints, true) == Result::Collision && downloads == 0);
  reset();
  put("/.crossdink/languages/de.YAML", yaml);
  assert(discover(cat.kind, entry, source) == Result::Collision);
  reset();
  put("/.crossdink/languages/DE.yaml", "_language_code: \"FR\"\n_language_name: \"Francais\"\nSTR_TEST: \"Salut\"\n");
  assert(discover(cat.kind, entry, source) == Result::Collision);
  reset();
  put("/.crossdink/languages/mine.yaml.old", yaml + "# old\n");
  assert(discover(cat.kind, entry, source) == Result::Ok && source.status == Status::Missing);
  assert(source.recoveryPending);
  assert(files.count("/.crossdink/languages/mine.yaml.old") && !files.count("/.crossdink/languages/mine.yaml"));
  assert(download(cat, entry, endpoints, false) == Result::Ok && files.at("/.crossdink/languages/mine.yaml") == yaml);
  reset();
  put("/.crossdink/languages/DE.yaml", yaml);
  put("/.crossdink/languages/DE.yaml.old", "backup");
  failRemove.insert("/.crossdink/languages/DE.yaml.old");
  assert(download(cat, entry, endpoints, true) == Result::Io && downloads == 0 &&
         files.at("/.crossdink/languages/DE.yaml") == yaml);
  reset();
  put("/.crossdink/languages/DE.yaml", yaml + "# original\n");
  response[response.size() - 2] = 'X';
  assert(download(cat, entry, endpoints, true) == Result::Invalid &&
         files.at("/.crossdink/languages/DE.yaml") == yaml + "# original\n");
  reset();
  sourceValid = false;
  assert(download(cat, entry, endpoints, false) == Result::Invalid && !files.count("/.crossdink/languages/DE.yaml"));
  reset();
  put("/.crossdink/languages/DE.yaml", yaml);
  beforeValidation = [] { files["/.crossdink/languages/DE.yaml"] += "# concurrent edit\n"; };
  assert(download(cat, entry, endpoints, true) == Result::Collision &&
         files.at("/.crossdink/languages/DE.yaml") == yaml + "# concurrent edit\n");
  reset();
  put("/.crossdink/languages/DE.yaml", yaml);
  failClose = true;
  assert(discover(cat.kind, entry, source) == Result::Io);
  reset();
  put("/.crossdink/languages/DE.yaml", yaml);
  failRead = true;
  assert(discover(cat.kind, entry, source) == Result::Io);
  reset();
  directories.insert("/.crossdink/languages");
  failIteration = true;
  assert(discover(cat.kind, entry, source) == Result::Io);
  reset();
  Callbacks callbacks;
  callbacks.cancelled = [](void*) { return true; };
  assert(download(cat, entry, endpoints, false, callbacks) == Result::Cancelled);
  reset();
  cancelledTransfer = true;
  assert(download(cat, entry, endpoints, false) == Result::Cancelled);
  reset();
  put("/.crossdink/languages/DE.yaml", yaml + "# old\n");
  failRename.insert("/.crossdink/languages/DE.yaml.part");
  assert(download(cat, entry, endpoints, true) == Result::Io &&
         files.at("/.crossdink/languages/DE.yaml") == yaml + "# old\n");
  reset();
  put("/.crossdink/languages/DE.yaml", yaml + "# old\n");
  failRemove.insert("/.crossdink/languages/DE.yaml.old");
  assert(download(cat, entry, endpoints, true) == Result::Io && files.at("/.crossdink/languages/DE.yaml") == yaml);
  reset();
  // CrossDink downloads translations only: a hyphenation entry is rejected before any transfer.
  cat = catalog(AssetCatalog::Kind::Hyphenation);
  assert(download(cat, entry, endpoints, false) == Result::Invalid && downloads == 0);
  for (const auto& reader : openReaders) assert(reader.second == 0);
  cleanupRetryTests(endpoints);
  companionGateTests(endpoints);
  std::cout << "Asset download tests passed (real SHA-256, storage fault injection, transport contract stub)\n";
}
