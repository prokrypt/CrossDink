#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>

#include <memory>
#include <string>
#include <vector>

#include "network/WifiPowerSaveGuard.h"

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  uint32_t modified;  // packed FAT date << 16 | time (local), 0 for folders or unknown
  bool isEpub;
  bool isDirectory;
};

// Reports a waiting request, so the transfer power hold can start before
// WebServer::handleClient() blocks reading the request body.
class PendingAwareWebServer final : public WebServer {
 public:
  using WebServer::WebServer;
  bool requestPending();
};

class CrossPointWebServer {
 public:
  struct WsUploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string filename;
    std::string lastCompleteName;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error = "";

    // Upload write buffer - batches small writes into larger SD card operations
    // 4KB is a good balance: large enough to reduce syscall overhead, small enough
    // to keep individual write times short and the server responsive
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;  // 4KB buffer
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    // Keeps the WiFi modem awake for the duration of the upload; STA-mode power
    // save otherwise adds beacon-interval latency to every small write round trip
    std::unique_ptr<WifiPowerSaveGuard> powerSaveGuard;

    UploadState() { buffer.resize(UPLOAD_BUFFER_SIZE); }
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }

  // True from the first byte of a request, upload or WebSocket message until
  // TRANSFER_LINGER_MS after the last one: CPU at full clock, no light sleep,
  // Wi-Fi modem awake. The linger keeps page loads and bursts fast.
  bool isTransferActive() const { return transferActive; }
  // STA mode only. Between transfers the modem sleeps between DTIM beacons
  // and the device light-sleeps between loop ticks; an AP must stay awake.
  bool allowsIdleSleep() const { return running && !apMode; }

  WsUploadStatus getWsUploadStatus() const;

  // True once after a client called POST /api/exit (the reply has been sent).
  bool consumeExitRequest() {
    const bool requested = exitRequestPending;
    exitRequestPending = false;
    return requested;
  }

  // Firmware .bin the exit request asked to flash (empty when none); cleared on read.
  std::string takeExitFlashPath() { return std::move(exitFlashPath); }

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<PendingAwareWebServer> server = nullptr;
  std::unique_ptr<WebSocketsServer> wsServer = nullptr;
  bool running = false;
  bool exitRequestPending = false;  // set by POST /api/exit, consumed by the activity
  std::string exitFlashPath;        // optional `flash` argument of POST /api/exit
  bool apMode = false;              // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;

  static constexpr unsigned long TRANSFER_LINGER_MS = 2000;
  bool transferActive = false;
  unsigned long lastTransferMs = 0;
  void noteTransferActivity();
  void updateTransferIdle();

  // WebSocket upload state
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  static void wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
  void abortWsUpload(const char* tag);

  // File scanning
  using FileVisitor = void (*)(const FileInfo& info, void* context);
  bool scanFiles(const char* path, FileVisitor visitor, void* context) const;
  String formatFileSize(size_t bytes) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleStyleCss() const;
  void handleLogo() const;
  void handleNotFound() const;
  void handleStatus() const;
#if CROSSDINK_PSRAM_LOG
  void handlePsramLog() const;
#endif
  void handleExit();
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontUpload();
  void handleFontUploadData();
  void handleFontDelete();

  // Font upload state
  struct FontUploadState {
    HalFile file;
    std::string familyName;
    std::string filePath;
    bool valid = false;
    size_t bytesWritten = 0;
    static constexpr size_t BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer;
    size_t bufferPos = 0;

    FontUploadState() { buffer.resize(BUFFER_SIZE); }
  } fontUpload;

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
};
