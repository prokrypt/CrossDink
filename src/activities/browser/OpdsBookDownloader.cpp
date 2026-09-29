#include "OpdsBookDownloader.h"

#include <Logging.h>
#include <ZipFile.h>

#include <utility>

#ifndef SIMULATOR

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {
// Same stack as the page prefetch (wolfSSL handshake plus the HTTP client),
// plus headroom for the EPUB check (ZipFile) after the last byte. Internal
// RAM, held only while a download runs.
constexpr uint32_t DOWNLOAD_STACK_BYTES = 14 * 1024;
constexpr size_t DOWNLOAD_BUFFER_SIZE = 2048;
// PSRAM, held only while a download runs: one SD write per 32 KB instead of
// one per TLS record.
constexpr size_t DOWNLOAD_WRITE_BUFFER_BYTES = 32 * 1024;
constexpr size_t RX_LOG_STEP_BYTES = 1024 * 1024;
}  // namespace

OpdsBookDownloader::~OpdsBookDownloader() {
  cancel();
  join();
}

bool OpdsBookDownloader::start(Request&& request) {
  if (running()) return false;

  job = std::move(request);
  outcome = HttpDownloader::HTTP_ERROR;
  bytesDone.store(0, std::memory_order_release);
  bytesTotal.store(0, std::memory_order_release);
  firstByteSeen.store(false, std::memory_order_release);
  cancelRequested.store(false, std::memory_order_release);
  if (!task.start([](void* self) { static_cast<OpdsBookDownloader*>(self)->run(); }, this, DOWNLOAD_STACK_BYTES,
                  "OpdsDownload")) {
    LOG_ERR("OPDS", "Download task could not start");
    return false;
  }
  return true;
}

void OpdsBookDownloader::run() {
  LOG_DBG("OPDS", "Downloading: %s -> %s", job.url.c_str(), job.path.c_str());
  const unsigned long startMs = millis();

  HttpDownloader::DownloadOptions options;
  options.shouldCancel = [this]() { return cancelRequested.load(std::memory_order_acquire); };
  options.bufferSize = DOWNLOAD_BUFFER_SIZE;
  options.transport = HttpDownloader::Transport::WOLFSSL;
  options.authorizationOrigin = job.authorizationOrigin;
  options.stageAsPart = true;
  options.checkFreeSpace = true;
  options.writeBufferBytes = DOWNLOAD_WRITE_BUFFER_BYTES;
  // A response with no Content-Length can end early and still look complete;
  // a truncated EPUB has no central directory to find container.xml in.
  options.validate = [](const std::string& path) {
    ZipFile zip(path);
    size_t size = 0;
    return zip.getInflatedFileSize("META-INF/container.xml", &size);
  };

  // A dropped link mid-download otherwise shows only as a stall.
  const wifi_event_id_t disconnectEvent = WiFi.onEvent(
      [](arduino_event_id_t, arduino_event_info_t info) {
        LOG_ERR("OPDS", "Wi-Fi disconnected during download (reason=%u)",
                static_cast<unsigned>(info.wifi_sta_disconnected.reason));
      },
      ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  size_t nextRxLog = RX_LOG_STEP_BYTES;
  unsigned long firstByteMs = 0;
  outcome = HttpDownloader::downloadToFile(
      job.url, job.path,
      [this, startMs, &nextRxLog, &firstByteMs](const size_t downloaded, const size_t total) {
        if (!firstByteSeen.load(std::memory_order_relaxed)) {
          // Everything before this is DNS, TLS, redirects and the server
          // preparing the file.
          firstByteMs = millis();
          LOG_DBG("OPDS", "First byte after %lu ms (total=%zu)", firstByteMs - startMs, total);
          firstByteSeen.store(true, std::memory_order_release);
        }
        if (downloaded >= nextRxLog) {
          const unsigned long elapsed = millis() - firstByteMs;
          LOG_INF("OPDS", "rx %zu/%zu KB, %lu KB/s, rssi=%d", downloaded / 1024, total / 1024,
                  elapsed > 0 ? static_cast<unsigned long>(downloaded / 1024 * 1000 / elapsed) : 0UL,
                  WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
          while (nextRxLog <= downloaded) nextRxLog += RX_LOG_STEP_BYTES;
        }
        bytesTotal.store(total, std::memory_order_release);
        bytesDone.store(downloaded, std::memory_order_release);
      },
      nullptr, job.username, job.password, std::move(options));
  WiFi.removeEvent(disconnectEvent);

  LOG_DBG("OPDS", "Download task done: result=%d in %lu ms, stack free=%u", static_cast<int>(outcome),
          millis() - startMs, static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

#else  // SIMULATOR

// The simulator has no network: the activity fakes a download screen instead.
OpdsBookDownloader::~OpdsBookDownloader() = default;
bool OpdsBookDownloader::start(Request&&) { return false; }
void OpdsBookDownloader::run() {}

#endif  // SIMULATOR
