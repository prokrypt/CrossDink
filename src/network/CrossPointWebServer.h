#pragma once

#include <HalStorage.h>
#include <Knobs.h>
#include <NetworkUdp.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "network/WifiPowerSaveGuard.h"

// WebSocketsServer whose shutdown cannot stall: close() writes a close frame
// to each client and retries for up to WEBSOCKETS_TCP_TIMEOUT (5 s) per client
// when a browser has stopped reading. Dropping the sockets first bounds the
// shutdown to the socket closes; browsers see an abnormal closure (1006).
class BoundedCloseWebSocketsServer : public WebSocketsServer {
 public:
  using WebSocketsServer::WebSocketsServer;
  void closeWithoutHandshake() {
#ifndef SIMULATOR
    for (auto& client : _clients) {
      if (client.tcp) clientDisconnect(&client);
    }
#endif
    close();
  }
};

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

  // Start the web server (call after WiFi is connected). Requests are served
  // on a dedicated task on the worker core; the server objects belong to that
  // task until stop() returns. `logOnly` serves just /api/psram-log (Goodies >
  // Wi-Fi remote): no file, settings, status, WebSocket or discovery handlers,
  // so nothing touches the SD card or I2C behind other screens.
  void begin(bool logOnly = false);

  // A running log-only STA server takes on every route (files, settings,
  // WebSocket, discovery) without closing its socket: File Transfer and
  // Calibre adopt the Goodies remote's server. False, still log-only or
  // stopped, when it can't (not log-only, AP mode, task start failed).
  bool upgradeToFull();

  // Stop the web server. Waits for the serving task to finish its current
  // request (an in-flight WebDAV PUT is aborted), so never call it from a
  // request handler. See waitForServeTask() for the time limits.
  void stop();

  // Check if server is running
  bool isRunning() const { return running.load(std::memory_order_acquire); }

  // True from the first byte of a request, upload or WebSocket message until
  // TRANSFER_LINGER_MS after the last one: CPU at full clock, no light sleep,
  // Wi-Fi modem awake. The linger keeps page loads and bursts fast. Log tail
  // and status polls end the hold they took without lingering.
  bool isTransferActive() const { return transferActive.load(std::memory_order_relaxed); }
  // True while a request has been served for over 100 ms or within `tailMs`
  // of the last request, upload chunk or WebSocket message. Log tail and
  // status polls never count. Unlike isTransferActive() it has no power
  // linger, so UI feedback can stop soon after data stops.
  bool isMovingData(unsigned long tailMs) const {
    const unsigned long startMs = requestStartMs.load(std::memory_order_relaxed);
    return (startMs != 0 && millis() - startMs >= 100) ||
           millis() - lastTransferMs.load(std::memory_order_relaxed) < tailMs;
  }
  // STA mode only. Between transfers the modem sleeps between DTIM beacons
  // and the device light-sleeps between polls; an AP must stay awake.
  bool allowsIdleSleep() const { return isRunning() && !apMode; }

  WsUploadStatus getWsUploadStatus() const;

  // True once after a client called POST /api/exit (the reply has been sent).
  bool consumeExitRequest() { return exitRequestPending.exchange(false, std::memory_order_acq_rel); }

  // Firmware .bin the exit request asked to flash (empty when none); cleared on read.
  std::string takeExitFlashPath();

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  void registerFullRoutes();
  void startWsAndUdp();
  bool startServeTask(bool psramStack);
  std::unique_ptr<PendingAwareWebServer> server = nullptr;
  std::unique_ptr<BoundedCloseWebSocketsServer> wsServer = nullptr;
  std::atomic<bool> running{false};
  std::atomic<bool> exitRequestPending{false};  // set by POST /api/exit, consumed by the activity
  std::string exitFlashPath;                    // optional `flash` argument of POST /api/exit; stateMutex
  bool apMode = false;                          // true when running in AP mode, false for STA mode
  bool logOnly_ = false;                        // begin(true): just the remote's routes
  uint16_t port = 80;
  uint16_t wsPort = 81;  // WebSocket port
  NetworkUDP udp;
  bool udpActive = false;

  static KNOB_ALIAS(TRANSFER_LINGER_MS, transferLingerMs);  // Goodies > Knobs
  std::atomic<bool> transferActive{false};
  std::atomic<unsigned long> lastTransferMs{0};
  std::atomic<unsigned long> requestStartMs{0};  // handleClient() is serving a request since; 0 = none
  bool pollRequest = false;                      // serving task: the request is a poll
  void noteTransferActivity();
  void updateTransferIdle();
  void endTransferHold();
  void releasePollHold();
  // Idle STA modem sleep: MAX_MODEM for the log-only remote (Settings > Max
  // Wi-Fi powersave, listen interval 3), else MIN_MODEM.
  void setIdleModemSleep();
  // Idle STA: blocks until one of this server's sockets is readable or
  // IDLE_POLL_MS passes (to notice stop requests). False on a timeout.
  bool waitForTraffic();
  // Serving task, logged once a minute while idle-capable ([WEB] idle 60s).
  struct IdleStats {
    uint32_t traffic, spurious, timeouts, holds, logWaits, logWoken;
  };
  mutable IdleStats idleStats{};
  unsigned long idleStatsMs = 0;
  void logIdleStats();

  // Serving task. It owns server and wsServer between begin() and stop().
  // Same stack as Arduino's loopTask, which used to run these handlers.
  static constexpr uint32_t SERVER_TASK_STACK_BYTES = 8192;
  // Idle STA: the select() timeout that notices stop requests (traffic wakes
  // the task at once), and the poll when no socket can be watched.
  static KNOB_ALIAS(IDLE_POLL_MS, serverIdlePollMs);  // Goodies > Knobs, as the next
  static KNOB_ALIAS(ACTIVE_PASSES_PER_TICK, serverActivePasses);
  TaskHandle_t serverTask = nullptr;
  // Log-only server (Goodies remote): stack in PSRAM. None of its handlers
  // touch the SD card, and /api/ota's flash writes run on firmware_flash's
  // internal-stack worker. Such a task parks at exit; stop() deletes it.
  bool serverTaskPsram = false;
  SemaphoreHandle_t serverStopped = nullptr;
  std::atomic<bool> stopRequested{false};
  // Serving task: what it is in ("http", "ws", "udp", "select", "tick"), for stop()'s log.
  std::atomic<const char*> servePhase{"start"};
  // stop(): STOP_GRACE_MS for the current request, then this server's
  // connections are shut down (unblocks a stalled upload or a dead peer).
  // Past STOP_GIVE_UP_MS the device restarts instead of hanging the screen.
  static constexpr uint32_t STOP_GRACE_MS = 500;
  static constexpr uint32_t STOP_GIVE_UP_MS = 15000;
  // Waits for the serving task to exit and deletes it; false when it is stuck.
  bool waitForServeTask();
  void logStopWait(unsigned long waitedMs) const;
  // fn(fd, localPort) for each open socket on this server's ports.
  template <typename Fn>
  void forEachSocket(Fn fn) const;
  static void serverTaskMain(void* param);
  void serveUntilStopped();
  bool handleClient();  // true when it served a request

  // Guards exitFlashPath and wsStatus, which the activity reads.
  SemaphoreHandle_t stateMutex = nullptr;
  WsUploadStatus wsStatus;
  void publishWsStatus();

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
  void handleLiteStatus() const;  // log-only server: no I2C or SD reads
#if CROSSDINK_PSRAM_LOG
  void handlePsramLog() const;
  void handleRemoteCmd() const;
  void handleScreenshot() const;
  void handleApiDownload() const;
  void handleApiUpload();
  void handleApiUploadPost();
  void handleOtaData() const;
  void handleOtaDone() const;
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
  void handleGetStatusBars() const;
  void handlePostStatusBars();

  // Font management handlers
  void handleFontsPage() const;
  void handleLogsPage() const;
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
