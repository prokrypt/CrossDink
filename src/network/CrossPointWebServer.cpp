#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <BoardConfig.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <Epub.h>
#include <FsHelpers.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PerfLog.h>
#include <PsramLog.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#ifndef SIMULATOR
#include <EnvironmentSensor.h>
#include <FreeInkDisplay.h>
#include <esp_heap_caps.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_wifi.h>
#include <freertos/idf_additions.h>
#include <lwip/sockets.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <iterator>

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "FirmwareFlasher.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "QuickActions.h"
#include "SdCardFontSystem.h"
#include "SerialRemote.h"
#include "SettingsList.h"
#include "SilentRestart.h"
#include "TaskCores.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/LogoPng.generated.h"
#include "html/LogsPageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/StyleCss.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "network/NetworkName.h"
#include "network/SdWriteBehind.h"
#include "util/BatteryLog.h"
#include "util/BatteryLogSum.h"
#include "util/BookCacheUtils.h"
#include "util/BootReason.h"
#include "util/BuildInfo.h"
#include "util/DeviceIdentity.h"
#include "util/DeviceSecurity.h"
#include "util/FontFamilyLabel.h"
#include "util/ReaderStatusBarJson.h"
#include "util/StringUtils.h"

namespace {
// While alive, plain malloc of any size tries PSRAM first (restores the
// sdkconfig threshold on end() or scope exit). Global: keep the window short.
struct PsramSmallAllocScope {
#if CONFIG_SPIRAM_USE_MALLOC && !defined(SIMULATOR)
  bool active = true;
  PsramSmallAllocScope() { heap_caps_malloc_extmem_enable(0); }
  ~PsramSmallAllocScope() { end(); }
  void end() {
    if (!active) return;
    active = false;
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  }
#else
  void end() {}
#endif
};
// Folders/files to hide from the web interface file browser.
// Dot-prefixed items are hidden unless showHiddenFiles is enabled.
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

bool isTwoFingerSwipeSetting(const SettingInfo& setting) {
  return setting.nameId == StrId::STR_TWO_FINGER_SWIPE_UP || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN ||
         setting.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT;
}

bool isSwipeActionSetting(const SettingInfo& setting) {
  return isTwoFingerSwipeSetting(setting) || setting.nameId == StrId::STR_LEFT_EDGE_UP ||
         setting.nameId == StrId::STR_LEFT_EDGE_DOWN || setting.nameId == StrId::STR_RIGHT_EDGE_UP ||
         setting.nameId == StrId::STR_RIGHT_EDGE_DOWN;
}

bool isWebEnumOptionAvailable(const SettingInfo& setting, size_t optionIndex) {
  if (optionIndex >= setting.enumValues.size()) return true;

  const StrId option = setting.enumValues[optionIndex];
  if (!SETTINGS.shouldTrackReadingStats()) {
    if (option == StrId::STR_READING_STATS) return false;
    if (setting.valuePtr == &CrossPointSettings::sleepScreen && optionIndex < setting.enumRawValues.size()) {
      const uint8_t raw = setting.enumRawValues[optionIndex];
      if (raw == CrossPointSettings::READING_STATS_SLEEP || raw == CrossPointSettings::MINIMAL_STATS_SLEEP)
        return false;
    }
  }
  if (option == StrId::STR_TOGGLE_TOUCHSCREEN && !gpio.hasTouch()) return false;

  if (!Frontlight.present()) {
    if (option == StrId::STR_TOGGLE_FRONTLIGHT ||
        (isSwipeActionSetting(setting) &&
         (option == StrId::STR_INCREASE_BRIGHTNESS || option == StrId::STR_DECREASE_BRIGHTNESS ||
          option == StrId::STR_INCREASE_WARMTH || option == StrId::STR_DECREASE_WARMTH))) {
      return false;
    }
    return setting.nameId != StrId::STR_REFRESH_FREQ || optionIndex >= setting.enumRawValues.size() ||
           setting.enumRawValues[optionIndex] != CrossPointSettings::REFRESH_NEVER;
  }

  return Frontlight.hasColorTemperature() || !isSwipeActionSetting(setting) ||
         (option != StrId::STR_INCREASE_WARMTH && option != StrId::STR_DECREASE_WARMTH);
}

uint8_t enumDisplayIndexForWeb(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) return rawValue;

  uint8_t displayIndex = 0;
  for (size_t optionIndex = 0; optionIndex < setting.enumRawValues.size(); ++optionIndex) {
    if (!isWebEnumOptionAvailable(setting, optionIndex)) continue;
    if (setting.enumRawValues[optionIndex] == rawValue) return displayIndex;
    ++displayIndex;
  }
  return 0;
}

bool isWebSettingAvailable(const SettingInfo& setting) {
  if (setting.category == StrId::STR_STATUS_BARS || setting.nameId == StrId::STR_HIDE_CLOCK) {
    return false;
  }
  if (setting.nameId == StrId::STR_SIDE_BUTTON_CHORD && !deviceSupportsSideButtonChord(gpio)) {
    return false;
  }

  const bool isTouchSetting =
      setting.nameId == StrId::STR_TOUCH_READER_CONTROLS || setting.nameId == StrId::STR_DISABLE_TOUCHSCREEN ||
      setting.nameId == StrId::STR_NEXT_PAGE || setting.nameId == StrId::STR_PREV_PAGE ||
      setting.nameId == StrId::STR_TAP_HIDE_STATUS_BAR || setting.nameId == StrId::STR_PINCH_FONT_RESIZE ||
      setting.nameId == StrId::STR_TWO_FINGER_ROTATION || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_UP ||
      setting.nameId == StrId::STR_TWO_FINGER_SWIPE_DOWN || setting.nameId == StrId::STR_TWO_FINGER_SWIPE_LEFT ||
      setting.nameId == StrId::STR_TWO_FINGER_SWIPE_RIGHT || setting.nameId == StrId::STR_LEFT_EDGE_UP ||
      setting.nameId == StrId::STR_LEFT_EDGE_DOWN || setting.nameId == StrId::STR_RIGHT_EDGE_UP ||
      setting.nameId == StrId::STR_RIGHT_EDGE_DOWN;
  if (isTouchSetting && !gpio.hasTouch()) {
    return false;
  }

  const bool isMultiTouchSetting = setting.nameId == StrId::STR_PINCH_FONT_RESIZE ||
                                   setting.nameId == StrId::STR_TWO_FINGER_ROTATION || isTwoFingerSwipeSetting(setting);
  if (isMultiTouchSetting && !gpio.supportsMultiTouch()) {
    return false;
  }

  const bool isFrontlightSetting = setting.nameId == StrId::STR_BRIGHTNESS || setting.nameId == StrId::STR_WARMTH ||
                                   setting.nameId == StrId::STR_FRONTLIGHT ||
                                   setting.nameId == StrId::STR_RESTORE_LIGHT_ON_WAKE ||
                                   setting.nameId == StrId::STR_FRONTLIGHT_SCHEDULE ||
                                   setting.nameId == StrId::STR_START || setting.nameId == StrId::STR_END;
  if (isFrontlightSetting && !Frontlight.present()) {
    return false;
  }

  const bool isFrontlightScheduleSetting = setting.nameId == StrId::STR_FRONTLIGHT_SCHEDULE ||
                                           setting.nameId == StrId::STR_START || setting.nameId == StrId::STR_END;
  if (isFrontlightScheduleSetting && !halClock.isAvailable()) {
    return false;
  }

  if (!halClock.isAvailable()) {
    switch (setting.nameId) {
      case StrId::STR_HIDE_CLOCK:
      case StrId::STR_CLOCK_OUTSIDE_READER:
      case StrId::STR_AUTO_BACKUP_STATS:
      case StrId::STR_CLOCK_UTC_OFFSET:
      case StrId::STR_CLOCK_DST:
      case StrId::STR_CLOCK_FORMAT:
      case StrId::STR_DATE_FORMAT:
      case StrId::STR_DATE_SEPARATOR:
      case StrId::STR_CLOCK_SYNCED:
        return false;
      default:
        break;
    }
  }

  return true;
}

// Streams a font-catalog JSON response in bounded pieces. This avoids holding
// both an ArduinoJson document and its serialized String in the fragmented
// network heap, and gives WiFi a chance to drain each piece before the next.
class FontListJsonWriter {
 public:
  explicit FontListJsonWriter(WebServer& server) : server_(server) {}

  void append(const char* text) { append(text, strlen(text)); }

  void append(const char* text, size_t textLength) {
    while (textLength > 0) {
      if (length_ == sizeof(buffer_)) flush();
      const size_t copyLength = std::min(textLength, sizeof(buffer_) - length_);
      memcpy(buffer_ + length_, text, copyLength);
      length_ += copyLength;
      text += copyLength;
      textLength -= copyLength;
    }
  }

  void appendUnsigned(unsigned long value) {
    char number[16];
    const int length = snprintf(number, sizeof(number), "%lu", value);
    append(number, static_cast<size_t>(length));
  }

  void appendJsonString(const char* value) {
    append("\"");
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) {
      switch (*p) {
        case '\"':
          append("\\\"");
          break;
        case '\\':
          append("\\\\");
          break;
        case '\b':
          append("\\b");
          break;
        case '\f':
          append("\\f");
          break;
        case '\n':
          append("\\n");
          break;
        case '\r':
          append("\\r");
          break;
        case '\t':
          append("\\t");
          break;
        default:
          if (*p < 0x20) {
            static constexpr char kHexDigits[] = "0123456789ABCDEF";
            const char escaped[] = {'\\', 'u', '0', '0', kHexDigits[*p >> 4], kHexDigits[*p & 0x0F]};
            append(escaped, sizeof(escaped));
          } else {
            append(reinterpret_cast<const char*>(p), 1);
          }
          break;
      }
    }
    append("\"");
  }

  void flush() {
    if (length_ == 0) return;
    server_.sendContent(buffer_, length_);
    length_ = 0;
  }

 private:
  static constexpr size_t BUFFER_SIZE = 192;
  WebServer& server_;
  char buffer_[BUFFER_SIZE];
  size_t length_ = 0;
};

// WebSocket upload state
HalFile wsUploadFile;

// Write-behind for WebSocket uploads (SdWriteBehind.h): frames land in PSRAM
// while a task on the other core writes the previous 32 KB to wsUploadFile.
static bool writeWsUploadChunk(void*, const uint8_t* data, size_t len) { return wsUploadFile.write(data, len) == len; }
SdWriteBehind wsWriteBehind;
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
// A phone browser drops the socket when its tab is hidden. The upload is
// parked (file kept open) for this long so a START for the same file resumes
// at wsUploadReceived instead of starting over.
constexpr unsigned long WS_RESUME_GRACE_MS = 60000;
unsigned long wsUploadParkedAt = 0;  // 0 = not parked
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;
// Keeps the WiFi modem awake for the duration of a WS upload; STA-mode power
// save otherwise adds beacon-interval latency to every small write round trip
std::unique_ptr<WifiPowerSaveGuard> wsUploadPowerSaveGuard;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool isProtectedPath(const String& path) {
  if (SerialRemote::isTokenPath(path.c_str())) return true;
  // Hidden/system items stay out of the default file-manager view. Enabling
  // Show Hidden Files intentionally makes them fully manageable.
  if (SETTINGS.showHiddenFiles) return false;

  // Check every segment so a hidden parent also protects its descendants.
  int start = 0;
  while (start < (int)path.length()) {
    if (path.charAt(start) == '/') {
      start++;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) end = path.length();

    String segment = path.substring(start, end);

    if (segment.startsWith(".")) return true;

    for (const auto* item : HIDDEN_ITEMS) {
      if (segment.equals(item)) return true;
    }

    start = end + 1;
  }

  return false;
}
#ifndef SIMULATOR
const char* displayControllerName(const BoardConfig::DisplayController controller) {
  switch (controller) {
    case BoardConfig::DisplayController::SSD1677:
      return "SSD1677";
    case BoardConfig::DisplayController::UC8253:
      return "UC8253";
    case BoardConfig::DisplayController::ED2208:
      return "ED2208";
    case BoardConfig::DisplayController::LgfxEpd:
      return "LgfxEpd";
    case BoardConfig::DisplayController::IT8951:
      return "IT8951";
    case BoardConfig::DisplayController::UC8279:
      return "UC8279";
    case BoardConfig::DisplayController::UC8179:
      return "UC8179";
    case BoardConfig::DisplayController::UC8279C:
      return "UC8279C";
  }
  return "unknown";
}

const char* touchControllerName(const BoardConfig::TouchController controller) {
  switch (controller) {
    case BoardConfig::TouchController::None:
      return "none";
    case BoardConfig::TouchController::Chsc6x:
      return "CHSC6x";
    case BoardConfig::TouchController::Gt911:
      return "GT911";
    case BoardConfig::TouchController::Ft5x06:
      return "FT5x06";
    case BoardConfig::TouchController::Ft6336u:
      return "FT6336U";
    case BoardConfig::TouchController::Gslx680:
      return "GSLX680";
  }
  return "unknown";
}

// Bytes of the running app image, from its segment headers. Reading the
// headers is cheap, unlike ESP.getSketchSize() which hashes the whole image,
// and the result can't change until the next boot.
uint32_t runningAppImageSize() {
  static uint32_t size = 0;
  if (size != 0) return size;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) return 0;
  const esp_partition_pos_t pos = {running->address, running->size};
  esp_image_metadata_t meta;
  if (esp_image_get_metadata(&pos, &meta) == ESP_OK) size = meta.image_len;
  return size;
}
#endif

// ISO 8601 UTC, e.g. "2026-09-26T19:30:00Z". Hand-rolled because strftime()
// and sniprintf() pull ~20 KB of newlib (tzset, sscanf, integer printf) into an
// image that has no other user of them. buf needs 21 bytes.
void formatUtc(char* buf, const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
               const uint8_t minute, const uint8_t second) {
  const auto put = [&buf](unsigned value, const int width, const char sep) {
    for (int i = width - 1; i >= 0; --i) {
      buf[i] = static_cast<char>('0' + value % 10);
      value /= 10;
    }
    buf += width;
    *buf++ = sep;
  };
  put(year, 4, '-');
  put(month, 2, '-');
  put(day, 2, 'T');
  put(hour, 2, ':');
  put(minute, 2, ':');
  put(second, 2, 'Z');
  *buf = '\0';
}

}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() {
  stop();
  if (stateMutex) vSemaphoreDelete(stateMutex);
  if (serverStopped) vSemaphoreDelete(serverStopped);
}

void CrossPointWebServer::begin(const bool logOnly) {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;
  logOnly_ = logOnly;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  // The server objects, route handlers and their strings are small blocks that
  // plain malloc keeps in internal RAM (SPIRAM_MALLOC_ALWAYSINTERNAL). None is
  // used by DMA, an ISR or with the flash cache off, so place them in PSRAM
  // while the server is built (~7 KB of internal heap). The serving task's
  // stack is created later with internal RAM, and lwIP picks its own caps.
  PsramSmallAllocScope psramSmallAllocs;
  server.reset(new PendingAwareWebServer(port));

  // STA mode starts idle: the modem sleeps between DTIM beacons and the
  // device may light-sleep between polls until a request arrives, which
  // switches both back to full power (noteTransferActivity). An AP must stay
  // awake to beacon, so it keeps the modem on throughout.
  transferActive.store(false, std::memory_order_relaxed);
  if (apMode) WiFi.setSleep(false);
  setIdleModemSleep();
  powerManager.setRadioIdleSleepAllowed(!apMode);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Add Access-Control-Allow-* headers to every response so web-based clients
  // and PWAs on other origins can use the HTTP API. Preflight OPTIONS requests
  // are answered in handleNotFound().
  server->enableCORS(true);

  // Setup routes
#if CROSSDINK_PSRAM_LOG
  server->on("/api/psram-log", HTTP_GET, [this] {
    releasePollHold();
    handlePsramLog();
  });
#endif
#if CROSSDINK_SERIAL_REMOTE
  server->on("/api/cmd", HTTP_POST, [this] { handleRemoteCmd(); });
  server->on("/api/screenshot", HTTP_ANY, [this] { handleScreenshot(); });
  server->on("/api/ota", HTTP_POST, [this] { handleOtaDone(); }, [this] { handleOtaData(); });
  // Token-gated SD file transfer, in log-only mode too: the web UI handlers
  // behind a remote-token check (docs/serial-remote.md).
  server->on("/api/download", HTTP_GET, [this] { handleApiDownload(); });
  server->on("/api/upload", HTTP_POST, [this] { handleApiUploadPost(); }, [this] { handleApiUpload(); });
#endif
  server->on("/api/status", HTTP_GET, [this] {
    releasePollHold();
    logOnly_ ? handleLiteStatus() : handleStatus();
  });
  // Watchers probe routes this server lacks: a 404 is a poll too, so it must
  // not hold full power for transferLingerMs.
  server->onNotFound([this] {
    releasePollHold();
    handleNotFound();
  });
  if (logOnly) {
    // Nothing else: no SD access behind other screens beyond the token-gated
    // /api/download and /api/upload, and /api/status's
    // battery and sensor I2C reads would race touch polling there.
    const char* remoteHeaders[] = {"X-Token"};
    server->collectHeaders(remoteHeaders, 1);
    server->begin();
  } else {
    registerFullRoutes();
    server->begin();
    startWsAndUdp();
  }
  psramSmallAllocs.end();

  // Do not subscribe the serving task to the task watchdog. Arduino WebServer
  // permits five-second client and ACK waits, which can consume the entire
  // default watchdog window on a weak connection even while the CPU idle task
  // is healthy. The system idle-task watchdog still detects real CPU stalls.

  if (!stateMutex) stateMutex = xSemaphoreCreateMutex();
  if (!serverStopped) serverStopped = xSemaphoreCreateBinary();
  stopRequested.store(false, std::memory_order_relaxed);
  running.store(true, std::memory_order_release);
  if (!startServeTask(logOnly)) return;

  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  wsWriteBehind.abort();  // the writer task must be done with the file first
  // Explicit close() required: file-scope global persists beyond function scope
  wsUploadFile.close();
  String filePath = wsUploadPath;
  if (!filePath.endsWith("/")) filePath += "/";
  filePath += wsUploadFileName;
  if (Storage.remove(filePath.c_str())) {
    LOG_DBG(tag, "Deleted incomplete upload: %s", filePath.c_str());
  } else {
    LOG_DBG(tag, "Failed to delete incomplete upload: %s", filePath.c_str());
  }
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsUploadParkedAt = 0;
  wsLastProgressSent = 0;
  wsUploadPowerSaveGuard.reset();
}

void CrossPointWebServer::registerFullRoutes() {
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });
  server->on("/style.css", HTTP_GET, [this] { handleStyleCss(); });
  server->on("/logo.png", HTTP_GET, [this] { handleLogo(); });

  server->on("/api/exit", HTTP_POST, [this] { handleExit(); });
#if CROSSDINK_GOODIES && !defined(SIMULATOR)
  // Battery log rows still in PSRAM, after the last row of /debug/logs/battery.csv.
  server->on("/api/battery-pending", HTTP_GET, [this] {
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, "text/csv", "");
    BatteryLog::forEachPending(
        [](void* s, const char* data, const uint32_t len) { static_cast<WebServer*>(s)->sendContent(data, len); },
        server.get());
    server->sendContent("");
  });
  // Goodies > Battery & stats' saved counts (battery.sum) and the log file + offset
  // they end at, so the web Battery tab counts on from them; 404 when there are none.
  server->on("/api/battery-sum", HTTP_GET, [this] {
    auto p = makeUniqueNoThrow<BatteryLogParser>();
    int file = 0;
    uint32_t offset = 0;
    if (!p || !BatteryLogSum::load(*p, file, offset)) {
      server->send(404, "text/plain", "no battery.sum");
      return;
    }
    const auto& s = p->st;
    JsonDocument doc;
    doc["file"] = file;
    doc["offset"] = offset;
    doc["first"] = s.first;
    doc["last"] = s.last;
    doc["reset"] = s.reset;
    doc["coldBoots"] = s.coldBoots;
    doc["restarts"] = s.restarts;
    doc["wakes"] = s.wakes;
    doc["falseWakes"] = s.falseWakes;
    doc["awakeS"] = s.awakeS;
    doc["asleepS"] = s.asleepS;
    doc["chargedEpoch"] = s.chargedEpoch;
    doc["chargeFromC"] = s.chargeFromC;
    doc["chargeFromFine"] = s.chargeFromFine;
    doc["chargeToC"] = s.chargeToC;
    doc["chargeToFine"] = s.chargeToFine;
    doc["charging"] = s.charging;
    doc["run"] = s.run;
    doc["runFine"] = s.runFine;
    const auto pair = [&doc](const char* key, const auto& v) {
      JsonArray a = doc[key].to<JsonArray>();
      a.add(v[0]);
      a.add(v[1]);
    };
    pair("battS", s.battS);
    pair("dropC", s.dropC);
    pair("coarseC", s.coarseC);
    pair("errC", s.errC);
    pair("netC", s.netC);
    pair("netCoarseC", s.netCoarseC);
    doc["prevEpoch"] = p->prev.epoch;
    doc["prevAwake"] = p->prev.awake;
    doc["prevC"] = p->prevC;
    doc["prevFine"] = p->prevFine;
    doc["prevUsb"] = p->prevUsb;
    String json;
    serializeJson(doc, json);
    server->send(200, "application/json", json);
  });
#endif
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });
  server->on("/api/status-bars", HTTP_GET, [this] { handleGetStatusBars(); });
  server->on("/api/status-bars", HTTP_POST, [this] { handlePostStatusBars(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/logs", HTTP_GET, [this] { handleLogsPage(); });
  server->on("/battery", HTTP_GET, [this] { handleLogsPage(); });  // same page, Battery tab
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth",      "Destination", "Overwrite",     "If",
                              "Lock-Token", "Timeout",     "If-None-Match", "X-Token"};
  server->collectHeaders(davHeaders, 8);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
}

void CrossPointWebServer::startWsAndUdp() {
  // Start WebSocket server for fast binary uploads
  wsServer.reset(new BoundedCloseWebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);
}

bool CrossPointWebServer::startServeTask(const bool psramStack) {
  // Internal-RAM stack (8 KB) only while the server runs: handlers write to
  // the SD card, and task stacks must stay reachable while flash is busy.
  // The log-only server's only SD handlers (/api/download, /api/upload) do no
  // flash writes, and the SDMMC driver bounces PSRAM buffers through its DMA
  // buffer, so its stack goes to PSRAM (internal fallback). UI core: on core 0
  // with Wi-Fi, lwIP and the loop, uploads held core 0 at 92% (WebServer 36%)
  // while core 1 did ~20%. The upload writer takes core 0.
  // Set before the task can run: serverTaskMain reads it to decide how to exit.
  serverTaskPsram = psramStack;
  if (serverTaskPsram &&
      (!stateMutex || !serverStopped ||
       xTaskCreatePinnedToCoreWithCaps(serverTaskMain, "WebServer", SERVER_TASK_STACK_BYTES, this, 2, &serverTask,
                                       TaskCores::kUi, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)) {
    serverTaskPsram = false;
  }
  if (!stateMutex || !serverStopped ||
      (!serverTaskPsram && xTaskCreatePinnedToCore(serverTaskMain, "WebServer", SERVER_TASK_STACK_BYTES, this, 2,
                                                   &serverTask, TaskCores::kUi) != pdPASS)) {
    LOG_ERR("WEB", "Failed to start web server task");
    serverTask = nullptr;
    stop();
    return false;
  }

  return true;
}

bool CrossPointWebServer::upgradeToFull() {
  if (!running || !server || !logOnly_ || apMode) return false;
  // Park the log-only serving task. The listening socket stays open, so a
  // client arriving meanwhile waits in lwIP's backlog instead of being refused.
  stopRequested.store(true, std::memory_order_release);
  if (serverTask) {
    xSemaphoreTake(serverStopped, portMAX_DELAY);
    if (serverTaskPsram) vTaskDeleteWithCaps(serverTask);  // frees the PSRAM stack
    serverTask = nullptr;
  }
  {
    PsramSmallAllocScope psramSmallAllocs;
    registerFullRoutes();
    startWsAndUdp();
  }
  logOnly_ = false;
  if (!isTransferActive()) setIdleModemSleep();  // MIN_MODEM: full routes want quicker replies
  stopRequested.store(false, std::memory_order_relaxed);
  // Full handlers write the SD card: internal-RAM stack, as begin(false).
  if (!startServeTask(/*psramStack=*/false)) return false;
  LOG_INF("WEB", "log-only server upgraded to full in place");
  return true;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running.load() ? 1 : 0, server.get());
    return;
  }

  running.store(false, std::memory_order_release);
  // Hand the server back to this task: the serving task finishes its current
  // request (bounded by WebServer's client timeouts), then exits.
  stopRequested.store(true, std::memory_order_release);
  if (serverTask) {
    xSemaphoreTake(serverStopped, portMAX_DELAY);
    if (serverTaskPsram) vTaskDeleteWithCaps(serverTask);  // frees the PSRAM stack
    serverTask = nullptr;
  }
  if (transferActive.exchange(false, std::memory_order_relaxed)) powerManager.endBackgroundWork();
  powerManager.setRadioIdleSleepAllowed(false);
  WiFi.setSleep(false);

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress WebSocket upload and remove partial file
  if (wsUploadInProgress && wsUploadFile) {
    abortWsUpload("WEB");
  }

  // Stop WebSocket server
  if (wsServer) {
    wsServer->closeWithoutHandshake();
    wsServer.reset();
    wsInstance = nullptr;
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  server->stop();

  // Brief delay before deletion
  delay(10);

  server.reset();

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::serverTaskMain(void* param) {
  auto* self = static_cast<CrossPointWebServer*>(param);
  const bool parks = self->serverTaskPsram;
  self->serveUntilStopped();  // last touch of self
  // A PSRAM stack cannot be freed by its own task: wait for stop() to delete it.
  if (parks) {
    for (;;) vTaskSuspend(nullptr);
  }
  vTaskDelete(nullptr);
}

void CrossPointWebServer::serveUntilStopped() {
  LOG_DBG("WEB", "Serving on core %d", xPortGetCoreID());
  bool readable = false;  // the last idle wait ended on socket traffic
  while (!stopRequested.load(std::memory_order_acquire)) {
    if (allowsIdleSleep() && !isTransferActive()) {
      // Idle STA: one pass per wake, blocked in between so tickless idle can
      // light-sleep. A request found here switches to transfer mode.
      const bool served = handleClient();
      logIdleStats();
      if (isTransferActive()) continue;
      if (readable && !served) {
        // Woke on a socket that had nothing for the server (an EOF it has
        // not closed yet, a discovery packet): poll once so it can't spin.
        ++idleStats.spurious;
        readable = false;
        vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
      } else {
        readable = waitForTraffic();
      }
      continue;
    }
    for (int i = 0; i < ACTIVE_PASSES_PER_TICK && !stopRequested.load(std::memory_order_relaxed); i++) {
      handleClient();
    }
    // Not yield(): lower-priority workers and IDLE0 need the core too.
    vTaskDelay(1);
  }
  PerfLog::noteTaskExit("WebServer");
  xSemaphoreGive(serverStopped);
}

std::string CrossPointWebServer::takeExitFlashPath() {
  if (!stateMutex) return {};
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  std::string path = std::move(exitFlashPath);
  exitFlashPath.clear();
  xSemaphoreGive(stateMutex);
  return path;
}

bool CrossPointWebServer::handleClient() {
  static bool loggedActive = false;

  // Check running flag FIRST before accessing server
  if (!isRunning()) {
    return false;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return false;
  }

  // Confirm once that the handler loop is running.
  if (!loggedActive) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    loggedActive = true;
  }

  // Take the power hold before handleClient(): it reads a whole upload or
  // sends a whole download in one blocking call.
  const bool pending = server->requestPending();
  if (pending) {
    const unsigned long lastDataMs = lastTransferMs;
    noteTransferActivity();
    lastTransferMs = lastDataMs;  // stamped below unless it turns out to be a poll
    pollRequest = false;
    requestStartMs.store(millis() | 1, std::memory_order_relaxed);
  }
  server->handleClient();
  if (pending) {
    if (!pollRequest) lastTransferMs = millis();
    requestStartMs.store(0, std::memory_order_relaxed);
  }

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }
  if (wsUploadParkedAt && millis() - wsUploadParkedAt > WS_RESUME_GRACE_MS) {
    LOG_DBG("WS", "Resume grace expired");
    abortWsUpload("WS");
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = NET_HOSTNAME;
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }

  updateTransferIdle();
  publishWsStatus();
  return pending;
}

bool PendingAwareWebServer::requestPending() {
  // A new connection waiting in accept(), or bytes on the kept-alive one. An
  // idle keep-alive connection does not count, so it cannot pin full power.
  return _server.hasClient() || _currentClient.available() > 0;
}

void CrossPointWebServer::noteTransferActivity() {
  lastTransferMs = millis();
  if (isTransferActive()) return;
  transferActive.store(true, std::memory_order_relaxed);
  // Holds the CPU frequency lock, which also keeps light sleep off, until
  // updateTransferIdle() or stop() ends the hold.
  powerManager.beginBackgroundWork();
  if (!apMode) WiFi.setSleep(false);
  ++idleStats.holds;
}

void CrossPointWebServer::updateTransferIdle() {
  if (!isTransferActive()) return;
  if (wsUploadInProgress) {
    lastTransferMs = millis();
    return;
  }
  if (millis() - lastTransferMs < TRANSFER_LINGER_MS) return;
  endTransferHold();
}

void CrossPointWebServer::endTransferHold() {
  transferActive.store(false, std::memory_order_relaxed);
  // The main loop drops the CPU lock on its next idle tick.
  powerManager.endBackgroundWork();
  setIdleModemSleep();
}

void CrossPointWebServer::setIdleModemSleep() {
  if (apMode) return;
#ifndef SIMULATOR
  const bool maxModem = logOnly_ && SETTINGS.maxWifiPowersave;
  WiFi.setSleep(maxModem ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
  // listen_interval goes out in the association request: a change applies at
  // the next (re)association. Arduino's WiFi.begin() resets it to 0 (= 3).
  static uint8_t loggedMode = 0xFF, loggedInterval = 0;
  constexpr uint8_t LISTEN_INTERVAL = 3;  // MAX_MODEM: beacons per wake
  wifi_config_t conf;
  uint8_t interval = 0;
  if (maxModem && esp_wifi_get_config(WIFI_IF_STA, &conf) == ESP_OK) {
    interval = conf.sta.listen_interval ? conf.sta.listen_interval : 3;
    if (interval != LISTEN_INTERVAL) {
      conf.sta.listen_interval = LISTEN_INTERVAL;
      if (esp_wifi_set_config(WIFI_IF_STA, &conf) == ESP_OK) interval = LISTEN_INTERVAL;
    }
  }
  // Logged only when it changes: a log line per poll would end every log long-poll.
  if (loggedMode != maxModem || (maxModem && loggedInterval != interval)) {
    loggedMode = maxModem;
    loggedInterval = interval;
    LOG_INF("WEB", "idle modem sleep %s%s, listen_interval %u (next association)", maxModem ? "MAX" : "MIN",
            logOnly_ ? " (log-only)" : "", interval);
  }
#else
  WiFi.setSleep(true);
#endif
}

bool CrossPointWebServer::waitForTraffic() {
#ifndef SIMULATOR
  // This server's sockets, found by local port: the HTTP listener and its
  // clients, the WebSocket listener and clients, the discovery UDP socket.
  fd_set fds;
  FD_ZERO(&fds);
  int maxFd = -1;
  for (int fd = LWIP_SOCKET_OFFSET; fd < LWIP_SOCKET_OFFSET + CONFIG_LWIP_MAX_SOCKETS; ++fd) {
    sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (lwip_getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) continue;
    // sin_port and sin6_port share an offset.
    const uint16_t local = ntohs(reinterpret_cast<const sockaddr_in*>(&addr)->sin_port);
    if (local != port && !(wsServer && local == wsPort) && !(udpActive && local == LOCAL_UDP_PORT)) continue;
    FD_SET(fd, &fds);
    maxFd = std::max(maxFd, fd);
  }
  if (maxFd >= 0) {
    timeval timeout = {static_cast<time_t>(IDLE_POLL_MS / 1000), static_cast<suseconds_t>(IDLE_POLL_MS % 1000 * 1000)};
    const int ready = lwip_select(maxFd + 1, &fds, nullptr, nullptr, &timeout);
    if (ready > 0) {
      ++idleStats.traffic;
      return true;
    }
    ++idleStats.timeouts;
    if (ready == 0) return false;
  }
#endif
  // No socket to watch (or select failed): poll.
  vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
  return false;
}

void CrossPointWebServer::logIdleStats() {
  const unsigned long now = millis();
  if (idleStatsMs == 0) idleStatsMs = now;
  if (now - idleStatsMs < 60000) return;
  idleStatsMs = now;
  const IdleStats st = idleStats;
  idleStats = {};
  LOG_INF("WEB", "idle 60s: wakes traffic=%lu spurious=%lu timeout=%lu holds=%lu log long-poll waits=%lu woken=%lu",
          static_cast<unsigned long>(st.traffic), static_cast<unsigned long>(st.spurious),
          static_cast<unsigned long>(st.timeouts), static_cast<unsigned long>(st.holds),
          static_cast<unsigned long>(st.logWaits), static_cast<unsigned long>(st.logWoken));
}

// Log tail and status polls are a few KB: end the hold now instead of
// lingering, so a poller cannot keep the device awake. This also ends a hold
// an earlier request left lingering (the log watcher GETs /api/status, a 404
// on the Wi-Fi remote, just before the log); the next request takes it again.
void CrossPointWebServer::releasePollHold() {
  // Not data either: the transfer light ignores it (a long-poll can wait a while).
  pollRequest = true;
  requestStartMs.store(0, std::memory_order_relaxed);
  if (!isTransferActive() || wsUploadInProgress) return;
  endTransferHold();
}

// Serving task: copy the upload state for the activity when it changed.
void CrossPointWebServer::publishWsStatus() {
  // wsStatus is written only here, so this task may read it unlocked.
  if (wsStatus.inProgress == wsUploadInProgress && wsStatus.received == wsUploadReceived &&
      wsStatus.total == wsUploadSize && wsStatus.lastCompleteAt == wsLastCompleteAt) {
    return;
  }
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  wsStatus.inProgress = wsUploadInProgress;
  wsStatus.received = wsUploadReceived;
  wsStatus.total = wsUploadSize;
  wsStatus.filename = wsUploadFileName.c_str();
  wsStatus.lastCompleteName = wsLastCompleteName.c_str();
  wsStatus.lastCompleteSize = wsLastCompleteSize;
  wsStatus.lastCompleteAt = wsLastCompleteAt;
  xSemaphoreGive(stateMutex);
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  if (!stateMutex) return {};
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  WsUploadStatus status = wsStatus;
  xSemaphoreGive(stateMutex);
  return status;
}

static void sendStaticContent(WebServer* server, const char* data, size_t len, const char* etag,
                              const char* contentType = "text/html") {
  server->sendHeader("ETag", etag);
  // Revalidate on each visit so firmware updates cannot leave stale pages cached.
  server->sendHeader("Cache-Control", "no-cache");
  if (server->header("If-None-Match") == etag) {
    server->send(304);
    return;
  }
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, contentType, data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendStaticContent(server.get(), HomePageHtml, sizeof(HomePageHtml), HomePageHtmlETag);
}

void CrossPointWebServer::handleJszip() const {
  sendStaticContent(server.get(), jszip_minJs, jszip_minJsCompressedSize, jszip_minJsETag, "application/javascript");
}

// Shared stylesheet and logo are referenced with a content-hashed ?v= query,
// so they can be cached aggressively: a new build changes the URL.
void CrossPointWebServer::handleStyleCss() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->send_P(200, "text/css", StyleCss, StyleCssCompressedSize);
}

void CrossPointWebServer::handleLogo() const {
  // Raw PNG (already compressed); no Content-Encoding.
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->send_P(200, "image/png", LogoPng, LogoPngSize);
}

void CrossPointWebServer::handleNotFound() const {
  // in AP mode, redirect unmatched browser/captive-portal requests to "/" so the OS auto-opens the browser
  // API requests (/api/*) still return 404 so XHR errors surface correctly
  // see https://en.wikipedia.org/wiki/Captive_portal#Detection
  if (apMode && !server->uri().startsWith("/api/")) {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "");
    return;
  }

  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleExit() {
  // Status code only, no body: 204 on success.
  // Refuse while a WebSocket upload is still streaming; leaving would abort it.
  if (wsUploadInProgress) {
    LOG_ERR("WEB", "/api/exit refused: WebSocket upload in progress");
    server->send(409);
    return;
  }
  // Optional flash=<path to .bin>: after leaving, the device opens SD Card
  // Firmware Update for that file and asks for confirmation before flashing.
  std::string flashPath;
  if (server->hasArg("flash")) {
    const String requested = normalizeWebPath(server->arg("flash"));
    String lower = requested;
    lower.toLowerCase();
    if (!lower.endsWith(".bin") || requested.length() >= MAX_SILENT_FIRMWARE_PATH || isProtectedPath(requested)) {
      LOG_ERR("WEB", "/api/exit refused: invalid flash path %s", requested.c_str());
      server->send(400);
      return;
    }
    if (!Storage.exists(requested.c_str())) {
      LOG_ERR("WEB", "/api/exit refused: %s not found", requested.c_str());
      server->send(404);
      return;
    }
    flashPath = requested.c_str();
  }
  LOG_DBG("WEB", "Exit requested via /api/exit (flash=%s)", flashPath.empty() ? "-" : flashPath.c_str());
  server->send(204);
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  exitFlashPath = std::move(flashPath);
  xSemaphoreGive(stateMutex);
  exitRequestPending.store(true, std::memory_order_release);
}

#if CROSSDINK_PSRAM_LOG
// Debug builds: the PSRAM log ring, oldest first, including lines from before
// the last software restarts. ?since=<offset> tails it: only bytes after that
// offset (a "[psram-log gap ...]" line marks any the ring overwrote first), and
// the X-Log-Next header is the offset for the next poll (X-Log-Oldest: the oldest byte held). &wait=<ms> (max 5000)
// holds an empty reply until new text arrives; that parks only this server task.
void CrossPointWebServer::handlePsramLog() const {
  EXT_RAM_NOINIT_ATTR static char chunk[1024];  // Static: debug-only, keeps 1 KB off the loop stack
  const bool tail = server->hasArg("since");
  const uint32_t since = tail ? strtoul(server->arg("since").c_str(), nullptr, 10) : 0;
  if (tail) {
    const uint32_t waitMs = std::min<uint32_t>(strtoul(server->arg("wait").c_str(), nullptr, 10), 5000);
    if (waitMs > 0 && PsramLog::end() == since) {
      ++idleStats.logWaits;
      if (PsramLog::waitForAppend(since, waitMs)) ++idleStats.logWoken;
    }
  }
  uint32_t cursor = PsramLog::oldest();
  // since > end: the ring restarted (power loss or deep sleep) after that offset.
  const bool restarted = tail && since > PsramLog::end();
  const uint32_t skipped = tail && !restarted && since < cursor ? cursor - since : 0;
  if (tail && !restarted && since > cursor) cursor = since;
  const uint32_t end = PsramLog::end();
  server->sendHeader("X-Log-Next", String(end));
  server->sendHeader("X-Log-Oldest", String(PsramLog::oldest()));  // Logs page shows next - oldest as the size
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "text/plain; charset=utf-8", "");
  size_t len = 0;
  if (!tail) {
    // Fresh identity header first: the ring may have wrapped past the boot lines.
    len = DeviceIdentity::formatLogHeader(chunk, sizeof(chunk));
  } else if (restarted) {
    len = snprintf(chunk, sizeof(chunk), "[psram-log restarted]\n");
  } else if (skipped > 0) {
    len = snprintf(chunk, sizeof(chunk), "[psram-log gap %lu bytes]\n", static_cast<unsigned long>(skipped));
  }
  if (len > 0) server->sendContent(chunk, len);
  while (cursor < end) {
    const uint32_t before = cursor;
    len = PsramLog::read(cursor, chunk, std::min<uint32_t>(sizeof(chunk), end - cursor));
    if (len == 0) break;
    if (cursor - len != before) {  // lapped while streaming: read() skipped ahead
      char gap[48];
      const int n = snprintf(gap, sizeof(gap), "\n[psram-log gap %lu bytes]\n",
                             static_cast<unsigned long>(cursor - len - before));
      server->sendContent(gap, n);
    }
    server->sendContent(chunk, len);
  }
  server->sendContent("");
}
#endif

#if CROSSDINK_SERIAL_REMOTE
// The requesting client, for the bad-token lockout.
static uint32_t clientIp(WebServer& server) { return static_cast<uint32_t>(server.client().remoteIP()); }

// Debug builds: runs one serial-remote command (docs/serial-remote.md) on the
// main task. Token and SD access stay on the main task too.
void CrossPointWebServer::handleRemoteCmd() const {
  if (server->arg("cmd") == "SCREENSHOT") return handleScreenshot();
  static char out[256];  // Static: server task only, keeps the reply off its stack
  const int status = SerialRemote::runFromOtherTask(server->arg("token").c_str(), server->arg("cmd").c_str(),
                                                    clientIp(*server), out, sizeof(out), 12000);
  server->send(status, "text/plain; charset=utf-8", out);
}

// Token check for the file routes: `token` argument or X-Token header, run as
// PING on the main task so the per-IP lockout applies. Returns the HTTP status.
static int checkRemoteToken(WebServer& server, char* out, const size_t outLen) {
  const String token = server.hasArg("token") ? server.arg("token") : server.header("X-Token");
  return SerialRemote::runFromOtherTask(token.c_str(), "PING", clientIp(server), out, outLen, 12000);
}

void CrossPointWebServer::handleApiDownload() const {
  static char out[32];
  const int status = checkRemoteToken(*server, out, sizeof(out));
  if (status != 200) return server->send(status, "text/plain; charset=utf-8", out);
  handleDownload();
}

namespace {
// /api/upload state, server task only. 0 = no file part seen yet.
int apiUploadStatus = 0;
char apiUploadOut[32];
}  // namespace

void CrossPointWebServer::handleApiUpload() {
  if (server->upload().status == UPLOAD_FILE_START) {
    apiUploadStatus = checkRemoteToken(*server, apiUploadOut, sizeof(apiUploadOut));
  }
  if (apiUploadStatus == 200) handleUpload(upload);
}

void CrossPointWebServer::handleApiUploadPost() {
  if (apiUploadStatus == 200) {
    handleUploadPost(upload);
  } else if (apiUploadStatus != 0) {
    server->send(apiUploadStatus, "text/plain; charset=utf-8", apiUploadOut);
  } else {
    server->send(400, "text/plain", "Missing file");
  }
  apiUploadStatus = 0;
}

namespace {
// /api/ota state, server task only.
bool otaAuthorized = false;
firmware_flash::Result otaResult = firmware_flash::Result::OK;
}  // namespace

// Debug builds: raw firmware upload (docs/serial-remote.md). The token is
// checked on the main task (as /api/cmd) before anything is erased; the image
// streams into the next OTA slot, verified in the same pass, and only a
// verified image switches otadata. No SD access.
void CrossPointWebServer::handleOtaData() const {
  const HTTPRaw& raw = server->raw();
  static char out[32];
  switch (raw.status) {
    case RAW_START:
      otaAuthorized = SerialRemote::runFromOtherTask(server->header("X-Token").c_str(), "PING", clientIp(*server), out,
                                                     sizeof(out), 12000) == 200;
      otaResult =
          otaAuthorized ? firmware_flash::streamBegin(server->clientContentLength()) : firmware_flash::Result::OK;
      break;
    case RAW_WRITE:
      if (otaAuthorized && otaResult == firmware_flash::Result::OK) {
        otaResult = firmware_flash::streamWrite(raw.buf, raw.currentSize);
      }
      break;
    case RAW_END:
      if (otaAuthorized && otaResult == firmware_flash::Result::OK) otaResult = firmware_flash::streamFinish();
      break;
    case RAW_ABORTED:
      firmware_flash::streamAbort();
      otaResult = firmware_flash::Result::READ_FAIL;
      break;
  }
}

void CrossPointWebServer::handleOtaDone() const {
  if (!otaAuthorized) {
    LOG_ERR("WEB", "/api/ota refused: bad token");
    server->send(403, "text/plain; charset=utf-8", "ERR:token");
    return;
  }
  otaAuthorized = false;
  LOG_INF("WEB", "/api/ota: %s, server task stack min free %u (%s)", firmware_flash::resultName(otaResult),
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), serverTaskPsram ? "PSRAM" : "internal");
  if (otaResult != firmware_flash::Result::OK) {
    firmware_flash::streamAbort();
    char msg[40];
    snprintf(msg, sizeof(msg), "ERR:OTA:%s", firmware_flash::resultName(otaResult));
    server->send(400, "text/plain; charset=utf-8", msg);
    return;
  }
  server->send(200, "text/plain; charset=utf-8", "OK:OTA rebooting");
  delay(200);  // let the reply leave before the restart
  // Restart on the main task, between loop passes, as CMD:REBOOT does.
  static char out[32];
  SerialRemote::runFromOtherTask(server->header("X-Token").c_str(), "REBOOT", clientIp(*server), out, sizeof(out),
                                 2000);
}

// Debug builds: the screen as a PGM (gray pass shown) or PBM, captured on the main task
// under the render lock (so never half-drawn) and sent from its static copy.
void CrossPointWebServer::handleScreenshot() const {
  static char out[64];
  const int status = SerialRemote::runFromOtherTask(server->arg("token").c_str(), "SCREENSHOT", clientIp(*server), out,
                                                    sizeof(out), 12000);
  if (status != 200) {
    server->send(status, "text/plain; charset=utf-8", out);
    return;
  }
  size_t len = 0;
  const uint8_t* pbm = SerialRemote::screenshot(len);
  server->setContentLength(len);
  server->send(200, pbm[1] == '5' ? "image/x-portable-graymap" : "image/x-portable-bitmap", "");
  server->sendContent(reinterpret_cast<const char*>(pbm), len);
}
#endif

void CrossPointWebServer::handleLiteStatus() const {
  JsonDocument doc;
  doc["version"] = AppVersion::version();
  doc["ip"] = WiFi.localIP().toString();
  doc["mode"] = "STA";
  doc["rssi"] = WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["logOnly"] = true;
  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = AppVersion::version();
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
#else
#ifdef SIMULATOR
  doc["device"] = "Simulator";
#else
  doc["device"] = BoardConfig::ACTIVE.name;
#endif
#endif

  char snBuf[33] = {0};
  bool valid = false;
#if !CONFIG_IDF_TARGET_ESP32
  // Classic ESP32's efuse table has no USER_DATA block (C3/S3 only)
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }
#endif

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  // Everything below is additive diagnostics; the keys above are read by the
  // web UI and upload tooling and keep their names and formats. Nothing here
  // touches the SD card beyond the capacity cached at mount.
  JsonObject build = doc["build"].to<JsonObject>();
  build["env"] = CROSSDINK_PIOENV;
  build["gitSha"] = BuildInfo::gitSha();
  build["gitDirty"] = BuildInfo::gitDirty();
  build["gitBranch"] = BuildInfo::gitBranch();
  build["buildNumber"] = BuildInfo::buildNumber();
  build["buildTime"] = BuildInfo::buildTime();

#ifndef SIMULATOR
  const BoardConfig::BoardProfile& board = BoardConfig::ACTIVE;
  doc["board"] = board.name;

  JsonObject display = doc["display"].to<JsonObject>();
  display["controller"] = displayControllerName(board.displayController);
  display["controllerVariant"] = board.displayControllerVariant;
  display["width"] = board.displayWidth;
  display["height"] = board.displayHeight;
  display["touch"] = touchControllerName(board.touch.controller);

  JsonObject chip = doc["chip"].to<JsonObject>();
  chip["model"] = ESP.getChipModel();
  chip["revision"] = ESP.getChipRevision();
  chip["cores"] = ESP.getChipCores();
  chip["cpuMHz"] = ESP.getCpuFreqMHz();
  chip["flashSize"] = ESP.getFlashChipSize();

#ifndef SIMULATOR
  const DeviceSecurity::State& sec = DeviceSecurity::get();
  if (sec.read) {
    JsonObject security = doc["security"].to<JsonObject>();
    security["flashEnc"] = sec.flashEnc;
    security["secureBoot"] = sec.secureBoot;
    security["usbSerialJtagDisabled"] = sec.usbSerialJtagDisabled;
    security["jtagDisabled"] = sec.jtagDisabled;
    security["download"] = sec.download;
    security["locked"] = sec.locked;
  }
#endif

  JsonObject memory = doc["memory"].to<JsonObject>();
  memory["freeHeap"] = ESP.getFreeHeap();
  memory["minFreeHeap"] = ESP.getMinFreeHeap();
  memory["maxAllocHeap"] = ESP.getMaxAllocHeap();
  memory["psramSize"] = ESP.getPsramSize();
  memory["freePsram"] = ESP.getFreePsram();

  JsonObject app = doc["app"].to<JsonObject>();
  const esp_partition_t* running = esp_ota_get_running_partition();
  app["partition"] = running ? running->label : "unknown";
  app["partitionSize"] = running ? running->size : 0;
  app["imageSize"] = runningAppImageSize();
#endif

  JsonObject boot = doc["boot"].to<JsonObject>();
  boot["resetReason"] = resetReasonName(esp_reset_reason());
  boot["wakeCause"] = wakeupCauseName(esp_sleep_get_wakeup_cause());
  boot["uptimeMs"] = millis();

  JsonObject battery = doc["battery"].to<JsonObject>();
  battery["percent"] = powerManager.getBatteryPercentage();
  battery["usb"] = gpio.isUsbConnectedCached();
#ifndef SIMULATOR
  {
    // Same function-local pattern as HalPowerManager: ACTIVE must be resolved
    // before the monitor is built. handleClient() runs on the main loop, so
    // this I2C/ADC read can't race the UI's own battery polling.
    static const BatteryMonitor monitor;
    battery["millivolts"] = monitor.readMillivolts();
    battery["charging"] = monitor.isCharging();
  }
#endif
#if CROSSDINK_GOODIES && !defined(SIMULATOR)
  {
    // The Goodies > Battery & stats counters (RTC memory), for the web Logs page.
    const BatteryLog::Stats& s = BatteryLog::stats();
    JsonObject st = battery["stats"].to<JsonObject>();
    st["now"] = BatteryLog::nowEpoch();
    st["boots"] = s.boots;
    st["wakes"] = s.wakes;
    st["awakeS"] = s.awakeS;
    st["asleepS"] = s.asleepS;
    st["chargedEpoch"] = s.chargedEpoch;
    st["chargedPct"] = s.chargedPct;
    st["falseWakes"] = s.falseWakes + s.pendingFalseWakes;
    st["battAwakeS"] = s.battAwakeS;
    st["battAsleepS"] = s.battAsleepS;
    st["dropAwakePct"] = s.dropAwakePct;
    st["dropAsleepPct"] = s.dropAsleepPct;
    const auto& c = HalDisplay::refreshCounts().n;
    JsonObject refresh = st["refresh"].to<JsonObject>();
    refresh["fast"] = c[HalDisplay::FAST_REFRESH];
    refresh["half"] = c[HalDisplay::HALF_REFRESH];
    refresh["full"] = c[HalDisplay::FULL_REFRESH];
    refresh["gray"] = c[HalDisplay::GRAY_PASSES];
    refresh["flash"] = c[HalDisplay::FLASHING];
    PerfLog::LightSleepStats ls;
    if (PerfLog::lightSleepStats(ls)) {
      JsonObject l = st["lightSleep"].to<JsonObject>();
      l["count"] = ls.sleeps;
      l["rejects"] = ls.rejects;
      l["pct"] = ls.sleepPct;
      l["upS"] = ls.upS;
      l["rejectCause"] = ls.rejectCause;
      l["rejectCauseName"] = ls.rejectCauseName;
    }
  }
#endif

  // Every temperature the board can report, in C; null when the read failed. The panel value is the UC8179's own
  // sensor, sampled after a refresh at most once a minute; ageMs says how old it is.
  JsonObject temps = doc["temperatures"].to<JsonObject>();
  const auto addTemp = [&temps](const char* role, const char* source, const bool known, const float celsius) {
    JsonObject t = temps[role].to<JsonObject>();
    t["source"] = source;
    if (known) {
      t["c"] = std::round(celsius * 10.0f) / 10.0f;
    } else {
      t["c"] = nullptr;
    }
  };
#ifdef SIMULATOR
  addTemp("chip", "simulator", false, 0.0f);
  addTemp("panel", "uc8179", false, 0.0f);
#else
  {
    const float chipC = temperatureRead();
    addTemp("chip", CONFIG_IDF_TARGET, !std::isnan(chipC), chipC);
    // Main-loop only, like the battery block above: the gauge shares its I2C bus
    // with touch and the RTC.
    static const BatteryMonitor gauge;
    int16_t gaugeDeciC = 0;
    const bool gaugeKnown = gauge.readTemperatureDeciC(gaugeDeciC);
    const auto& gaugeCfg = BoardConfig::ACTIVE.batteryGauge;
    const char* gaugeName = gaugeCfg.gaugeAddr == 0                                 ? "none"
                            : gaugeCfg.gaugeType == BoardConfig::GaugeType::Cw2017  ? "cw2017"
                            : gaugeCfg.gaugeType == BoardConfig::GaugeType::Bq27220 ? "bq27220"
                                                                                    : "axp2101";
    addTemp("battery", gaugeName, gaugeKnown, gaugeDeciC / 10.0f);
    static EnvironmentSensor ambient;
    static bool ambientTried = false;
    if (!ambientTried) {
      ambientTried = true;
      ambient.begin();
    }
    if (ambient.present()) {  // only boards that carry an SHT40 list it
      float ambientC = 0.0f;
      float humidity = 0.0f;
      const bool ambientKnown = ambient.read(ambientC, humidity);
      addTemp("ambient", "sht40", ambientKnown, ambientC);
    }
#if FREEINK_UC8179_PANEL_TEMP  // UC8179 boards only (not Sticky)
    int8_t panelC = 0;
    uint32_t panelAgeMs = 0;
    const bool panelKnown = freeink::uc8179PanelTemperature(panelC, panelAgeMs);
    addTemp("panel", "uc8179", panelKnown, panelC);
    if (panelKnown) temps["panel"]["ageMs"] = panelAgeMs;
#endif
  }
#endif

  JsonObject clock = doc["clock"].to<JsonObject>();
  char timeBuf[21];
  const time_t now = time(nullptr);
  // Anything before 2025 means the system clock was never set (no NTP/RTC).
  const bool systemTimeValid = now >= 1735689600;
  clock["systemTimeValid"] = systemTimeValid;
  if (systemTimeValid) {
    struct tm tm;
    gmtime_r(&now, &tm);
    formatUtc(timeBuf, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    clock["systemTime"] = timeBuf;
  }
  clock["rtc"] = halClock.isAvailable();
  uint16_t year;
  uint8_t month, day, hour, minute;
  if (halClock.getDateTime(year, month, day, hour, minute)) {
    formatUtc(timeBuf, year, month, day, hour, minute, 0);
    clock["rtcTime"] = timeBuf;
    clock["rtcTimeValid"] = year >= 2025;
  }

  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = apMode ? WiFi.softAPSSID() : WiFi.SSID();
  wifi["channel"] = WiFi.channel();
  wifi["hostname"] = WiFi.getHostname();
  wifi["mac"] = apMode ? WiFi.softAPmacAddress() : WiFi.macAddress();

  JsonObject sd = doc["sd"].to<JsonObject>();
  sd["ready"] = Storage.ready();
  sd["totalBytes"] = Storage.totalBytes();

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

bool CrossPointWebServer::scanFiles(const char* path, const FileVisitor visitor, void* context) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return false;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return false;
  }

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    if (visitor) {
      file.getName(name, sizeof(name));
      auto fileName = String(name);

      // Skip hidden items (starting with ".")
      bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

      // Treat OS/device metadata like other hidden items: keep it out of the
      // default view, but let users manage it when Show Hidden Files is enabled.
      if (!shouldHide && !SETTINGS.showHiddenFiles) {
        for (const auto* item : HIDDEN_ITEMS) {
          if (fileName.equals(item)) {
            shouldHide = true;
            break;
          }
        }
      }

      if (!shouldHide) {
        FileInfo info;
        info.name = fileName;
        info.isDirectory = file.isDirectory();

        if (info.isDirectory) {
          info.size = 0;
          info.modified = 0;
          info.isEpub = false;
        } else {
          info.size = file.size();
          info.modified = file.modificationTime();
          info.isEpub = isEpubFile(info.name);
        }

        visitor(info, context);
      }
    }

    file.close();
    yield();  // Yield to allow WiFi and other tasks to process during long scans
    file = root.openNextFile();
  }
  const bool complete = !FsHelpers::directoryIterationFailed(root);
  if (!complete) {
    LOG_ERR("WEB", "Directory listing failed before EOF: %s", path);
  }
  root.close();
  return complete;
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendStaticContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml), FilesPageHtmlETag);
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedPath(currentPath)) {
    server->send(403, "application/json", "[]");
    return;
  }

  // This response runs on the web-server task, so a TCP-sized heap buffer is
  // safer than adding 1.4KB to its stack. Allocation is fallible and retains
  // the old per-entry path as a low-memory fallback.
  constexpr size_t BATCH_CAPACITY = 1400;
  auto batch = makeUniqueNoThrow<char[]>(BATCH_CAPACITY);
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  // Check the iterator before committing to a streamed 200 response. Without
  // this pass, an SD read error is indistinguishable from a complete JSON list.
  if (!scanFiles(currentPath.c_str(), nullptr, nullptr)) {
    server->send(500, "application/json", "{\"error\":\"Directory listing failed\"}");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");

  struct FileListContext {
    WebServer* server;
    char* batch;
    size_t batchLen;
    char* output;
    JsonDocument* doc;
    bool seenFirst;
  } context{server.get(), batch.get(), 0, output, &doc, false};

  if (batch) {
    batch[context.batchLen++] = '[';
  } else {
    LOG_ERR("WEB", "OOM: file list batch buffer; falling back to per-entry sends");
    server->sendContent("[");
  }

  const bool complete = scanFiles(
      currentPath.c_str(),
      [](const FileInfo& info, void* rawContext) {
        auto& context = *static_cast<FileListContext*>(rawContext);
        context.doc->clear();
        (*context.doc)["name"] = info.name;
        (*context.doc)["size"] = info.size;
        (*context.doc)["isDirectory"] = info.isDirectory;
        (*context.doc)["isEpub"] = info.isEpub;
        if (info.modified != 0) (*context.doc)["mtime"] = info.modified;

        const size_t written = serializeJson(*context.doc, context.output, outputSize);
        if (written >= outputSize) {
          // JSON output truncated; skip this entry to avoid sending malformed JSON
          LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
          return;
        }

        const size_t required = written + (context.seenFirst ? 1 : 0);
        if (context.batch) {
          if (context.batchLen + required > BATCH_CAPACITY) {
            context.server->sendContent(context.batch, context.batchLen);
            context.batchLen = 0;
          }
          if (context.seenFirst) context.batch[context.batchLen++] = ',';
          memcpy(context.batch + context.batchLen, context.output, written);
          context.batchLen += written;
        } else {
          if (context.seenFirst) context.server->sendContent(",");
          context.server->sendContent(context.output);
        }
        context.seenFirst = true;
      },
      &context);

  if (!complete) {
    // A second-pass failure must not be closed into a valid partial array.
    server->client().stop();
    return;
  }

  if (batch) {
    if (context.batchLen + 1 > BATCH_CAPACITY) {
      server->sendContent(batch.get(), context.batchLen);
      context.batchLen = 0;
    }
    batch[context.batchLen++] = ']';
    server->sendContent(batch.get(), context.batchLen);
  } else {
    server->sendContent("]");
  }
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
#ifndef SIMULATOR
  client.clear();
#endif
  file.close();
}

// Upload start time is used for the completion throughput summary.
static unsigned long uploadStartTime = 0;

static bool flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    const size_t written = state.file.write(state.buffer.data(), state.bufferPos);

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    state.fileName = StringUtils::sanitizeFilename(upload.filename.c_str()).c_str();
    state.size = 0;
    state.success = false;
    state.error = "";
    uploadStartTime = millis();
    state.bufferPos = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    if (isProtectedPath(filePath)) {
      state.error = "Access denied to protected path";
      LOG_DBG("WEB", "[UPLOAD] FAILED: Access denied to protected path: %s", filePath.c_str());
      return;
    }

    // Check if file already exists.
    if (Storage.exists(filePath.c_str())) {
      state.error = "File already exists: " + state.fileName;
      LOG_DBG("WEB", "[UPLOAD] Collision: %s", filePath.c_str());
      return;
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
    if (!Storage.openFileForWrite("WEB", filePath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }

    state.powerSaveGuard = makeUniqueNoThrow<WifiPowerSaveGuard>();

  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.file && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          if (!flushUploadBuffer(state)) {
            state.error = "Failed to write to SD card - disk may be full";
            state.file.close();
            return;
          }
        }
      }

      state.size += upload.currentSize;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    state.powerSaveGuard.reset();
    if (state.file) {
      // Flush any remaining buffered data
      if (!flushUploadBuffer(state)) {
        state.error = "Failed to write final data to SD card";
      }
      state.file.close();

      if (state.error.isEmpty()) {
        state.success = true;
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);

        // Clear epub cache after uploading the file
        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        clearBookCachePreservingUserState(filePath.c_str());
        ImageFolderIndex::invalidateForPath(filePath.c_str());
        sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.powerSaveGuard.reset();
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) {
      state.file.close();
      // Try to delete the incomplete file
      String filePath = state.path;
      if (!filePath.endsWith("/")) filePath += "/";
      filePath += state.fileName;
      Storage.remove(filePath.c_str());
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String requestedName = server->arg("name");
  size_t leadingDots = 0;
  while (leadingDots < requestedName.length() && requestedName[leadingDots] == '.') {
    leadingDots++;
  }

  const String nameSuffix = requestedName.substring(leadingDots);
  bool suffixHasNameCharacter = false;
  for (size_t i = 0; i < nameSuffix.length(); ++i) {
    if (nameSuffix[i] != ' ' && nameSuffix[i] != '.') {
      suffixHasNameCharacter = true;
      break;
    }
  }
  if (!suffixHasNameCharacter || leadingDots >= StringUtils::kDefaultMaxFilenameBytes) {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }
  const size_t suffixBudget =
      StringUtils::kDefaultMaxFilenameBytes > leadingDots ? StringUtils::kDefaultMaxFilenameBytes - leadingDots : 0;
  const String sanitizedSuffix = StringUtils::sanitizeFilename(nameSuffix.c_str(), suffixBudget).c_str();
  String folderName = requestedName.substring(0, leadingDots);
  folderName += sanitizedSuffix;

  // Validate folder name
  if (folderName.isEmpty() || folderName == "book") {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  HalFile parent = Storage.open(parentPath.c_str());
  if (!parent || !parent.isDirectory()) {
    parent.close();
    server->send(404, "text/plain", "Parent directory does not exist");
    return;
  }
  parent.close();

  if (isProtectedPath(parentPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  // Allow creating a new hidden folder in a visible parent. Existing hidden
  // and system-managed paths remain protected unless Show Hidden Files is on.
  if (isProtectedPath(folderPath) && !folderName.startsWith(".")) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    const auto visibility = FsHelpers::directoryEntryVisibility(parentPath.c_str(), folderPath.c_str());
    if (visibility != FsHelpers::DirectoryEntryVisibility::Visible) {
      const bool rolledBack =
          visibility == FsHelpers::DirectoryEntryVisibility::Missing && Storage.rmdir(folderPath.c_str());
      LOG_ERR("WEB", "Created folder is not enumerable: %s (visibility=%u rollback=%d)", folderPath.c_str(),
              static_cast<unsigned>(visibility), rolledBack);
      server->send(500, "text/plain", "Folder could not be added to the directory listing");
      return;
    }
    ImageFolderIndex::invalidateForPath(folderPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }

  // Calculate new path to check if it's protected
  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (isProtectedPath(newPath) || SerialRemote::isTokenPath(itemPath.c_str(), true) ||
      SerialRemote::isTokenPath(newPath.c_str(), true)) {
    server->send(403, "text/plain", "Cannot rename to protected path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    ImageFolderIndex::invalidateForPath(itemPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    ImageFolderIndex::invalidateForPath(newPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (isProtectedPath(destPath)) {
    server->send(403, "text/plain", "Cannot move into protected folder");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (SerialRemote::isTokenPath(newPath.c_str())) {
    file.close();
    server->send(403, "text/plain", "Cannot move into protected folder");
    return;
  }
  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  clearBookCache(itemPath.c_str());
  const bool success = file.rename(newPath.c_str());
  file.close();

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    ImageFolderIndex::invalidateForPath(itemPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    ImageFolderIndex::invalidateForPath(newPath.c_str());
    sdFontSystem.markRegistryDirtyForPath(newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = normalizeWebPath(p.as<String>());

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      f.close();
      success = Storage.removeDir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      success = Storage.remove(itemPath.c_str());
      clearBookCache(itemPath.c_str());
    }

    if (!success) {
      LOG_ERR("WEB", "Failed to delete item: %s", itemPath.c_str());
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    } else {
      ImageFolderIndex::invalidateForPath(itemPath.c_str());
      sdFontSystem.markRegistryDirtyForPath(itemPath.c_str());
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendStaticContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml), SettingsPageHtmlETag);
}

void CrossPointWebServer::handleGetStatusBars() const {
  JsonDocument doc;
  writeReaderStatusBarJson(doc["top"].to<JsonObject>(), SETTINGS.readerStatusBar(ReaderStatusBarPosition::Top));
  writeReaderStatusBarJson(doc["bottom"].to<JsonObject>(), SETTINGS.readerStatusBar(ReaderStatusBarPosition::Bottom));
  doc["xtcMode"] = SETTINGS.xtcStatusBarMode;
  doc["clockAvailable"] = halClock.isAvailable();

  JsonObject labels = doc["labels"].to<JsonObject>();
  labels["top"] = tr(STR_TOP_STATUS_BAR);
  labels["bottom"] = tr(STR_BOTTOM_STATUS_BAR);
  labels["left"] = tr(STR_STATUS_BAR_LEFT);
  labels["center"] = tr(STR_CENTER);
  labels["right"] = tr(STR_STATUS_BAR_RIGHT);
  labels["percentageFormat"] = tr(STR_PERCENTAGE_FORMAT);
  labels["progressBar"] = tr(STR_PROGRESS_BAR);
  labels["thickness"] = tr(STR_PROGRESS_BAR_THICKNESS);
  labels["xtcMode"] = tr(STR_XTC_STATUS_BAR);
  labels["preview"] = tr(STR_PREVIEW);

  JsonArray options = doc["options"].to<JsonArray>();
  const auto addOption = [&options](ReaderStatusBarItem item, const std::string& label) {
    JsonObject option = options.add<JsonObject>();
    option["value"] = static_cast<uint8_t>(item);
    option["label"] = label;
  };
  addOption(ReaderStatusBarItem::Clock, tr(STR_STATUS_BAR_CLOCK));
  addOption(ReaderStatusBarItem::Battery, tr(STR_BATTERY));
  const auto combined = [](const char* first, const char* second) { return std::string(first) + " (" + second + ")"; };
  addOption(ReaderStatusBarItem::TimeLeftBook, combined(tr(STR_TIME_LEFT), tr(STR_BOOK)).c_str());
  addOption(ReaderStatusBarItem::TimeLeftChapter, combined(tr(STR_TIME_LEFT), tr(STR_CHAPTER)).c_str());
  addOption(ReaderStatusBarItem::ChapterPageCount, tr(STR_CHAPTER_PAGE_COUNT));
  addOption(ReaderStatusBarItem::StablePageNumber, tr(STR_STABLE_PAGE_NUMBERS));
  addOption(ReaderStatusBarItem::BookProgressPercentage, tr(STR_BOOK_PROGRESS_PERCENTAGE));
  addOption(ReaderStatusBarItem::TitleBook, combined(tr(STR_TITLE), tr(STR_BOOK)).c_str());
  addOption(ReaderStatusBarItem::TitleChapter, combined(tr(STR_TITLE), tr(STR_CHAPTER)).c_str());
  addOption(ReaderStatusBarItem::Empty, tr(STR_STATUS_BAR_EMPTY));

  const auto addLabels = [&doc](const char* name, std::initializer_list<StrId> ids) {
    JsonArray labels = doc[name].to<JsonArray>();
    for (const StrId id : ids) labels.add(I18N.get(id));
  };
  addLabels("percentageFormats", {StrId::STR_PERCENTAGE_FORMAT_WHOLE, StrId::STR_PERCENTAGE_FORMAT_ONE_DECIMAL,
                                  StrId::STR_PERCENTAGE_FORMAT_TWO_DECIMALS});
  addLabels("progressModes", {StrId::STR_BOOK, StrId::STR_CHAPTER, StrId::STR_HIDE});
  addLabels("thicknesses",
            {StrId::STR_PROGRESS_BAR_THIN, StrId::STR_PROGRESS_BAR_MEDIUM, StrId::STR_PROGRESS_BAR_THICK});
  addLabels("xtcModes", {StrId::STR_HIDE, StrId::STR_BOTTOM, StrId::STR_TOP, StrId::STR_STATUS_BAR_BOTH});

  String payload;
  serializeJson(doc, payload);
  server->send(200, "application/json", payload);
}

void CrossPointWebServer::handlePostStatusBars() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, server->arg("plain"))) {
    server->send(400, "text/plain", "Invalid JSON");
    return;
  }
  ReaderStatusBarsPayload bars;
  if (!CrossPointSettings::parseReaderStatusBars(doc.as<JsonVariantConst>(), bars)) {
    server->send(400, "text/plain", "Invalid status bar configuration");
    return;
  }
  {
    std::lock_guard<std::mutex> lock(SETTINGS.getMutex());
    const auto& previousTop = SETTINGS.topReaderStatusBar;
    if (previousTop.slots != bars.top.slots || previousTop.percentageFormat != bars.top.percentageFormat ||
        previousTop.progressBar != bars.top.progressBar ||
        previousTop.progressBarThickness != bars.top.progressBarThickness ||
        SETTINGS.xtcStatusBarMode != bars.xtcMode) {
      SETTINGS.legacyXtcTopUsesBottom = 0;
    }
    SETTINGS.topReaderStatusBar = bars.top;
    SETTINGS.bottomReaderStatusBar = bars.bottom;
    SETTINGS.xtcStatusBarMode = bars.xtcMode;
  }
  if (!SETTINGS.saveToFile()) {
    LOG_ERR("WEB", "Failed to save status bar configuration");
    server->send(500, "text/plain", "Failed to save status bars");
    return;
  }
  server->send(200, "text/plain", "Status bars saved");
}

void CrossPointWebServer::handleGetSettings() const {
  // The device settings UI needs an owned, mutable copy of the settings list.
  // The web API only reads it, so iterate the static base list directly rather
  // than copying its nested vectors and callbacks while WiFi is using the heap.
  sdFontSystem.refreshIfDirty();
  const auto& settings = getBaseSettingsList();
  const auto& fontFamilies = sdFontSystem.registry().getFamilies();
  const SdCardFontFamilyInfo* selectedSdFamily =
      SETTINGS.sdFontFamilyName[0] == '\0' ? nullptr : sdFontSystem.registry().findFamily(SETTINGS.sdFontFamilyName);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key || !isWebSettingAvailable(s)) continue;  // Skip ACTION-only and unavailable entries.

    doc.clear();
    doc["key"] = s.key;
    if (isSideButtonActionSetting(s)) {
      const bool up = settingKeyIs(s, "sideButtonUpShort") || settingKeyIs(s, "sideButtonUpLong");
      doc["name"] = sideButtonGroupLabel(up) + " " + I18N.get(s.nameId);
    } else {
      doc["name"] = I18N.get(s.nameId);
    }
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr == &CrossPointSettings::shortPwrBtn || s.valuePtr == &CrossPointSettings::longPwrBtn) {
          doc["footnotesIndex"] = enumDisplayIndexForWeb(s, CrossPointSettings::FOOTNOTES);
        } else if (s.valuePtr == &CrossPointSettings::longPressMenuAction ||
                   s.valuePtr == &CrossPointSettings::longPressBackAction) {
          doc["footnotesIndex"] = enumDisplayIndexForWeb(s, CrossPointSettings::LONG_MENU_FOOTNOTES);
        }
        if (s.nameId == StrId::STR_FONT_FAMILY && !fontFamilies.empty()) {
          uint8_t selected = SETTINGS.fontFamily < CrossPointSettings::BUILTIN_FONT_COUNT ? SETTINGS.fontFamily : 0;
          if (selectedSdFamily) {
            const auto it = std::find_if(
                fontFamilies.begin(), fontFamilies.end(),
                [](const SdCardFontFamilyInfo& family) { return family.name == SETTINGS.sdFontFamilyName; });
            if (it != fontFamilies.end()) {
              selected = static_cast<uint8_t>(CrossPointSettings::BUILTIN_FONT_COUNT +
                                              std::distance(fontFamilies.begin(), it));
            }
          }
          doc["value"] = selected;
        } else if (s.nameId == StrId::STR_FONT_SIZE && selectedSdFamily && s.valuePtr) {
          const auto sizes = selectedSdFamily->availableSizes();
          const uint8_t selectedPointSize = SETTINGS.*(s.valuePtr);
          const auto it = std::find(sizes.begin(), sizes.end(), selectedPointSize);
          doc["value"] = static_cast<int>(it == sizes.end() ? 0 : std::distance(sizes.begin(), it));
        } else if (s.valuePtr) {
          doc["value"] = static_cast<int>(enumDisplayIndexForWeb(s, SETTINGS.*(s.valuePtr)));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (s.nameId == StrId::STR_FONT_FAMILY && !fontFamilies.empty()) {
          constexpr auto builtinRange = BUILTIN_FONT_POINT_SIZE_RANGE;
          options.add(fontFamilyLabel(I18N.get(StrId::STR_LEXEND_DECA), builtinRange));
          options.add(fontFamilyLabel(I18N.get(StrId::STR_BITTER), builtinRange));
          for (const auto& family : fontFamilies) {
            options.add(fontFamilyLabel(family.name, fontFamilyPointSizeRange(family)));
          }
        } else if (s.nameId == StrId::STR_FONT_SIZE && selectedSdFamily) {
          const auto sizes = selectedSdFamily->availableSizes();
          for (const uint8_t pointSize : sizes) {
            char label[8];
            snprintf(label, sizeof(label), "%u pt", pointSize);
            options.add(label);
          }
        } else if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (size_t optionIndex = 0; optionIndex < s.enumValues.size(); ++optionIndex) {
            if (isWebEnumOptionAvailable(s, optionIndex)) {
              options.add(sideButtonOptionLabel(s, static_cast<uint8_t>(optionIndex)));
            }
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.value16Ptr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.value16Ptr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        // Passwords are write-only in the web UI. Returning the KOReader
        // value can expose credentials and, for legacy invalid data, emit
        // binary bytes that make the whole JSON response unparsable.
        if (strcmp(s.key, "koPassword") == 0) {
          doc["value"] = "";
        } else if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t prefixSize = seenFirst ? 1 : 0;
    if (prefixSize != 0) output[0] = ',';
    const size_t written = serializeJson(doc, output + prefixSize, outputSize - prefixSize);
    if (written >= outputSize - prefixSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    seenFirst = true;
    server->sendContent(output, written + prefixSize);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
  sdFontSystem.releaseRegistry();
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  sdFontSystem.refreshIfDirty();
  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;
  uint8_t CrossPointSettings::* twoFingerSwipeEdited = nullptr;

  for (const auto& s : settings) {
    if (!s.key || !isWebSettingAvailable(s)) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = enumRawValueForDisplayIndex(s, static_cast<uint8_t>(val));
            QuickActions::settingChanged(SETTINGS, s.valuePtr);
            if (s.valuePtr == &CrossPointSettings::twoFingerSwipeUp ||
                s.valuePtr == &CrossPointSettings::twoFingerSwipeDown ||
                s.valuePtr == &CrossPointSettings::twoFingerSwipeLeft ||
                s.valuePtr == &CrossPointSettings::twoFingerSwipeRight) {
              twoFingerSwipeEdited = s.valuePtr;
            }
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.value16Ptr) {
            SETTINGS.*(s.value16Ptr) = static_cast<uint16_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (std::strcmp(s.key, "deviceName") == 0 && (val.length() < CrossPointSettings::MIN_DEVICE_NAME_LENGTH ||
                                                      val.length() > CrossPointSettings::MAX_DEVICE_NAME_LENGTH)) {
          break;
        }
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  if (twoFingerSwipeEdited != nullptr) {
    CrossPointSettings::normalizeTwoFingerSwipeActions(SETTINGS, twoFingerSwipeEdited);
  }
  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
  sdFontSystem.releaseRegistry();
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    doc["filenameFormat"] = opdsFilenameFormatToJson(servers[i].filenameFormat);
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");
  opdsServer.filenameFormat = opdsFilenameFormatFromJson(doc["filenameFormat"] | "");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");
  const bool hasFilenameFormatField =
      doc["filenameFormat"].is<const char*>() || doc["filenameFormat"].is<std::string>();

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
    // Preserve existing values for fields older clients do not know how to send.
    if (existing && !hasPasswordField) {
      password = existing->password;
    }
    if (existing && !hasFilenameFormatField) {
      opdsServer.filenameFormat = existing->filenameFormat;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto credentials = WIFI_STORE.getCredentialSummaries();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = credentials[i].hasPassword;
    doc["isLastConnected"] = credentials[i].isLastConnected;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
    yield();  // Allow WiFi and other tasks to run during a slow send.
  }

  server->sendContent("]");
  server->sendContent("");
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }
    const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
    if (!credential) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credential->ssid;
    if (!hasPasswordField) {
      password = credential->password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }
  const auto ssid = WIFI_STORE.getSsidAt(static_cast<size_t>(idx));
  if (!ssid) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  if (!WIFI_STORE.removeCredential(*ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid->c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//      Server replies "READY", or "READY:<received>" to resume a dropped upload
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (type == WStype_CONNECTED || type == WStype_TEXT || type == WStype_BIN) noteTransferActivity();
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && wsUploadFile) {
        LOG_DBG("WS", "Upload parked at %u/%u bytes", (unsigned)wsUploadReceived, (unsigned)wsUploadSize);
        wsUploadClientNum = 255;
        wsUploadParkedAt = millis() | 1;
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);

      if (msg.startsWith("START:")) {
        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        // Same file as the parked upload: resume where it stopped.
        if (wsUploadParkedAt && firstColon > 0 && secondColon > 0 &&
            wsUploadFileName == StringUtils::sanitizeFilename(msg.substring(6, firstColon).c_str()).c_str() &&
            msg.substring(firstColon + 1, secondColon) == String(wsUploadSize) &&
            wsUploadPath == normalizeWebPath(msg.substring(secondColon + 1))) {
          LOG_DBG("WS", "Resuming upload at %u/%u bytes", (unsigned)wsUploadReceived, (unsigned)wsUploadSize);
          wsUploadClientNum = num;
          wsUploadParkedAt = 0;
          wsLastProgressSent = wsUploadReceived;
          wsServer->sendTXT(num, "READY:" + String(wsUploadReceived));
          break;
        }
        // Any other START drops a parked upload.
        if (wsUploadParkedAt) abortWsUpload("WS");

        // Reject any START while an upload is already active to prevent
        // leaking the open wsUploadFile handle (owning client re-START included)
        if (wsUploadInProgress) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = StringUtils::sanitizeFilename(msg.substring(6, firstColon).c_str()).c_str();
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          bool sizeValid = sizeToken.length() > 0;
          int digitStart = (sizeValid && sizeToken[0] == '+') ? 1 : 0;
          if (digitStart > 0 && sizeToken.length() < 2) sizeValid = false;
          for (int i = digitStart; i < (int)sizeToken.length() && sizeValid; i++) {
            if (!isdigit((unsigned char)sizeToken[i])) sizeValid = false;
          }
          if (!sizeValid) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadSize = sizeToken.toInt();
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          if (isProtectedPath(filePath)) {
            wsServer->sendTXT(num, "ERROR:Access denied to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          if (Storage.exists(filePath.c_str())) {
            LOG_DBG("WS", "Upload collision: %s", filePath.c_str());
            wsServer->sendTXT(num, "ERROR:File already exists: " + wsUploadFileName);
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          // Open file for writing
          sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
          if (!Storage.openFileForWrite("WS", filePath, wsUploadFile)) {
            wsServer->sendTXT(num, "ERROR:Failed to create file");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            // Explicit close() required: file-scope global persists beyond function scope
            wsUploadFile.close();
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            clearBookCachePreservingUserState(filePath.c_str());
            ImageFolderIndex::invalidateForPath(filePath.c_str());
            sdFontSystem.markRegistryDirtyForPath(filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsWriteBehind.begin(&writeWsUploadChunk, nullptr, "WsWriter", TaskCores::kWorker);
          wsUploadPowerSaveGuard = makeUniqueNoThrow<WifiPowerSaveGuard>();
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || !wsUploadFile || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      const bool written = wsWriteBehind.active() ? wsWriteBehind.append(payload, length)
                                                  : wsUploadFile.write(payload, length) == length;
      if (!written) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
        return;
      }

      wsUploadReceived += length;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        if (wsWriteBehind.active() && !wsWriteBehind.finish()) {
          abortWsUpload("WS");
          wsServer->sendTXT(num, "ERROR:Write failed - disk full?");
          return;
        }
        // Explicit close() required: file-scope global persists beyond function scope
        wsUploadFile.close();
        wsUploadInProgress = false;
        wsUploadClientNum = 255;

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();
        wsUploadPowerSaveGuard.reset();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        // Clear epub cache after uploading the file
        String filePath = wsUploadPath;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += wsUploadFileName;
        clearBookCachePreservingUserState(filePath.c_str());
        ImageFolderIndex::invalidateForPath(filePath.c_str());
        sdFontSystem.markRegistryDirtyForPath(filePath.c_str());

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleLogsPage() const {
  sendStaticContent(server.get(), LogsPageHtml, sizeof(LogsPageHtml), LogsPageHtmlETag);
}

void CrossPointWebServer::handleFontsPage() const {
  sendStaticContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml), FontsPageHtmlETag);
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  sdFontSystem.ensureRegistry();
  const auto& families = sdFontSystem.registry().getFamilies();

  // Send the catalog as it is enumerated. Building an ArduinoJson document and
  // then serializing it to a String keeps two complete copies in RAM, which
  // can exhaust the fragmented network heap with larger font collections.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  FontListJsonWriter json(*server);
  json.append("{\"families\":[");

  bool firstFamily = true;
  for (const auto& family : families) {
    // Hydrate and emit one family's paths at a time. Keeping every family's
    // paths resident is what made larger catalogs exhaust the X3 network heap.
    if (!family.ensureDetails()) continue;

    if (!firstFamily) json.append(",");
    firstFamily = false;

    json.append("{\"name\":");
    json.appendJsonString(family.name.c_str());
    json.append(",\"sizes\":[");

    bool firstSize = true;
    for (uint8_t s : family.availableSizes()) {
      if (!firstSize) json.append(",");
      firstSize = false;
      json.appendUnsigned(s);
    }
    json.append("],\"files\":[");

    bool firstFile = true;
    family.ensureDetails();
    for (const auto& file : family.files) {
      if (!firstFile) json.append(",");
      firstFile = false;

      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      json.append("{\"name\":");
      json.appendJsonString(name ? name + 1 : file.path.c_str());

      // Stat the file for size
      HalFile f;
      unsigned long fileSize = 0;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileSize = static_cast<unsigned long>(f.size());
        f.close();
      }

      json.append(",\"size\":");
      json.appendUnsigned(fileSize);
      json.append("}");
      json.flush();
      yield();
    }
    json.append("]}");
    json.flush();
    family.releaseDetails();
    yield();
  }

#if CROSSDINK_SCALABLE_FONTS
  json.append("],\"ttfSupported\":true,\"maxFamilies\":");
#else
  json.append("],\"ttfSupported\":false,\"maxFamilies\":");
#endif
  json.appendUnsigned(SdCardFontRegistry::MAX_SD_FAMILIES);
  json.append("}");
  json.flush();
  server->sendContent("");
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      String family = server->arg("family");
      fontUpload.file = HalFile();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a .cpfont basename of alphanumeric + hyphen + underscore. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[192];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;
      sdFontSystem.markRegistryDirty();

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.valid) break;

      // Validate the complete file after closing it; multipart chunks may
      // split the signature at any byte boundary.
#if CROSSDINK_SCALABLE_FONTS
      const auto& path = fontUpload.filePath;
      if (path.size() > 4 && strcasecmp(path.c_str() + path.size() - 4, ".ttf") == 0 &&
          fontUpload.bytesWritten + fontUpload.bufferPos + upload.currentSize > 2 * 1024 * 1024) {
        fontUpload.valid = false;
        LOG_ERR("WEB", "TTF exceeds 2 MiB limit");
        break;
      }
#endif

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      // Flush remaining buffer
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      if (fontUpload.file.isOpen()) {
        fontUpload.file.close();
      }

      if (fontUpload.valid) fontUpload.valid = FontInstaller::validateCpfontFile(fontUpload.filePath.c_str());
      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      if (fontUpload.file) {
        fontUpload.file.close();
      }
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.valid = false;
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Unsupported or damaged font file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  sdFontSystem.refreshIfDirty();
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
