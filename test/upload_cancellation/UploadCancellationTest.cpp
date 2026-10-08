// Handler bodies are extracted from production; network and SD I/O are deterministic fakes.
#include <strings.h>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define LOG_DBG(...) ((void)0)
#define LOG_ERR(...) ((void)0)
#define CROSSDINK_SCALABLE_FONTS 1

class String : public std::string {
 public:
  using std::string::string;
  using std::string::operator=;
  bool isEmpty() const { return empty(); }
  bool endsWith(const char* suffix) const {
    return size() >= strlen(suffix) && compare(size() - strlen(suffix), strlen(suffix), suffix) == 0;
  }
};
struct HalFile {
  bool opened = true;
  std::vector<uint8_t> bytes;
  explicit operator bool() const { return opened; }
  bool isOpen() const { return opened; }
  void close() { opened = false; }
  size_t write(const uint8_t* data, size_t size) {
    assert(opened);
    bytes.insert(bytes.end(), data, data + size);
    return size;
  }
};
struct StorageMock {
  std::vector<std::string> removed;
  bool remove(const char* path) {
    removed.emplace_back(path);
    return true;
  }
  bool rename(const char*, const char*) { return true; }
} Storage;
std::vector<std::string> parked;
void parkUploadPart(const String& part) { parked.push_back(part); }
struct FontSystem {
  void markRegistryDirtyForPath(const char*) {}
} sdFontSystem;
struct ImageFolderIndex {
  static void invalidateForPath(const char*) {}
};
unsigned cacheClears = 0;
void clearBookCachePreservingUserState(const char*) { ++cacheClears; }
unsigned long millis() { return 100; }
unsigned long uploadStartTime = 0;
struct FontInstaller {
  static unsigned validations;
  static bool validateCpfontFile(const char*) {
    ++validations;
    return true;
  }
};
unsigned FontInstaller::validations = 0;
struct WifiPowerSaveGuard {};
enum { UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload {
  int status = UPLOAD_FILE_WRITE;
  const uint8_t* buf = nullptr;
  size_t currentSize = 0;
};
struct Client {
  bool connected = true;
  void stop() { connected = false; }
};
struct Server {
  HTTPUpload body;
  Client socket;
  HTTPUpload& upload() { return body; }
  Client& client() { return socket; }
};
class CrossPointWebServer {
 public:
  struct UploadState {
    HalFile file;
    String path = "/books";
    String fileName = "test.epub";
    String error;
    String partPath;
    bool success = false;
    size_t size = 0;
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(UPLOAD_BUFFER_SIZE);
    size_t bufferPos = 0;
    std::unique_ptr<WifiPowerSaveGuard> powerSaveGuard = std::make_unique<WifiPowerSaveGuard>();
  } upload;
  struct FontUploadState {
    HalFile file;
    std::string filePath = "/fonts/Test/Regular.cpfont";
    bool valid = true;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(BUFFER_SIZE);
    size_t bufferPos = 0;
  } fontUpload;
  std::atomic<bool> running{true};
  std::atomic<bool> stopRequested{false};
  std::unique_ptr<Server> server = std::make_unique<Server>();
  bool dropUploadIfCancelled() const;
  void abortUpload(UploadState&) const;
  void abortFontUpload();
  void handleUpload(UploadState&) const;
  void handleFontUploadData();
  void beginStop() {  // stop()'s order
    running = false;
    stopRequested = true;
  }
};
#include "UploadHandlers.inc"

void send(CrossPointWebServer& server, bool font, int status, size_t bytes = 0) {
  static uint8_t body[8192] = {};
  assert(bytes <= sizeof(body));
  server.server->body = {status, body, bytes};
  if (font)
    server.handleFontUploadData();
  else
    server.handleUpload(server.upload);
}

int main() {
  for (bool font : {false, true}) {
    // Normal multi-chunk uploads still finish with every byte and no deletion,
    // also while upgradeToFull() parks the serving task (stopRequested, running).
    for (bool upgrading : {false, true}) {
      Storage.removed.clear();
      CrossPointWebServer server;
      server.stopRequested = upgrading;
      send(server, font, UPLOAD_FILE_WRITE, 6000);
      send(server, font, UPLOAD_FILE_WRITE, 77);
      send(server, font, UPLOAD_FILE_END);
      assert(Storage.removed.empty() && server.server->socket.connected);
      assert(font ? server.fontUpload.valid : server.upload.success);
      assert((font ? server.fontUpload.file.bytes.size() : server.upload.file.bytes.size()) == 6077);
      assert(!(font ? server.fontUpload.file.isOpen() : server.upload.file.isOpen()));
    }

    for (int cancelStatus : {UPLOAD_FILE_WRITE, UPLOAD_FILE_END}) {
      Storage.removed.clear();
      cacheClears = FontInstaller::validations = 0;
      CrossPointWebServer cancelled;
      send(cancelled, font, UPLOAD_FILE_WRITE, 5000);  // disk data plus unflushed tail
      cancelled.beginStop();
      send(cancelled, font, cancelStatus, 10);
      assert(!cancelled.server->socket.connected);
      assert(!(font ? cancelled.fontUpload.file.isOpen() : cancelled.upload.file.isOpen()));
      assert((font ? cancelled.fontUpload.bufferPos : cancelled.upload.bufferPos) == 0);
      assert(!(font ? cancelled.fontUpload.valid : cancelled.upload.success));
      assert(font || !cancelled.upload.powerSaveGuard);
      assert(Storage.removed.front() == (font ? "/fonts/Test/Regular.cpfont" : "/books/test.epub"));
      // Arduino may still dispatch END or ABORTED after the client is dropped.
      send(cancelled, font, UPLOAD_FILE_END);
      send(cancelled, font, UPLOAD_FILE_ABORTED);
      assert(!(font ? cancelled.fontUpload.valid : cancelled.upload.success));
      assert(cacheClears == 0 && FontInstaller::validations == 0);
    }
    // Unrequested transport abort still cleans up.
    CrossPointWebServer aborted;
    send(aborted, font, UPLOAD_FILE_WRITE, 80);
    send(aborted, font, UPLOAD_FILE_ABORTED);
    assert(!(font ? aborted.fontUpload.file.isOpen() : aborted.upload.file.isOpen()));
  }
  // A resumable upload's .part is parked for the retry, not deleted.
  Storage.removed.clear();
  CrossPointWebServer resumable;
  resumable.upload.partPath = "/books/test.epub.part";
  send(resumable, false, UPLOAD_FILE_WRITE, 5000);
  resumable.beginStop();
  send(resumable, false, UPLOAD_FILE_WRITE, 10);
  assert(Storage.removed.empty() && parked.size() == 1 && parked.front() == "/books/test.epub.part");
  assert(resumable.upload.partPath.isEmpty());
}
