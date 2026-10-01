#include "OpdsBookDownloader.h"

#include <Knobs.h>
#include <Logging.h>
#include <ZipFile.h>

#include <utility>

#include "util/UrlUtils.h"

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
// Book hosts can drop a long response without closing it (seen: mayberry.pub
// branch hosts stop ~65 s into a 32 MB book). Give up on a silent body after
// this long instead of the 60 s request timeout, then resume it.
KNOB_ALIAS(BODY_STALL_TIMEOUT_MS, opdsStallMs);  // Goodies > Knobs
// Automatic Range resumes per download before the Retry/Cancel prompt. Each
// must have made progress, so a dead server still fails after one attempt.
constexpr uint8_t MAX_AUTO_RESUMES = 4;
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
  // UI core: on the worker core it shared ~90% of core 0 with Wi-Fi, lwIP and
  // the loop and topped out at 250 KB/s while core 1 sat idle. The download
  // screen redraws rarely, so the render task loses little time-slicing with it.
  if (!task.start([](void* self) { static_cast<OpdsBookDownloader*>(self)->run(); }, this, DOWNLOAD_STACK_BYTES,
                  "OpdsDownload", true)) {
    LOG_ERR("OPDS", "Download task could not start");
    return false;
  }
  return true;
}

void OpdsBookDownloader::run() {
  LOG_DBG("OPDS", "Downloading: %s -> %s", UrlUtils::maskUserInfo(job.url).c_str(), job.path.c_str());
  const unsigned long startMs = millis();

  if (job.sizeOnly) {
    // The first progress report carries Content-Length and arrives with the
    // first chunk; shouldCancel then ends the request. A second chunk with no
    // length known yet ends it as well.
    bool firstChunk = true;
    HttpDownloader::DownloadOptions options;
    options.shouldCancel = [this]() { return cancelling() || total() > 0; };
    options.bufferSize = DOWNLOAD_BUFFER_SIZE;
    options.transport = HttpDownloader::Transport::WOLFSSL;
    options.authorizationOrigin = job.authorizationOrigin;
    HttpDownloader::streamUrl(
        job.url,
        [&firstChunk](const uint8_t*, size_t) {
          const bool more = firstChunk;
          firstChunk = false;
          return more;
        },
        [this](size_t, const size_t total) { bytesTotal.store(total, std::memory_order_release); }, job.username,
        job.password, std::move(options));
    LOG_INF("OPDS", "Size probe: %zu bytes in %lu ms", total(), millis() - startMs);
    return;
  }

  auto makeOptions = [this]() {
    HttpDownloader::DownloadOptions options;
    options.shouldCancel = [this]() { return cancelRequested.load(std::memory_order_acquire); };
    options.bufferSize = DOWNLOAD_BUFFER_SIZE;
    options.transport = HttpDownloader::Transport::WOLFSSL;
    options.authorizationOrigin = job.authorizationOrigin;
    options.stageAsPart = true;
    options.checkFreeSpace = true;
    options.writeBufferBytes = DOWNLOAD_WRITE_BUFFER_BYTES;
    options.preservePartial = true;
    options.resumePartial = job.resume;
    options.validator = &job.validator;
    // A response with no Content-Length can end early and still look complete;
    // a truncated EPUB has no central directory to find container.xml in.
    options.validate = [](const std::string& path) {
      ZipFile zip(path);
      size_t size = 0;
      return zip.getInflatedFileSize("META-INF/container.xml", &size);
    };
    options.stallTimeoutMs = BODY_STALL_TIMEOUT_MS;
    return options;
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
  const auto onProgress = [this, startMs, &nextRxLog, &firstByteMs](const size_t downloaded, const size_t total) {
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
  };

  outcome =
      HttpDownloader::downloadToFile(job.url, job.path, onProgress, nullptr, job.username, job.password, makeOptions());
  // A failed attempt keeps its .part file: continue it from where it stopped
  // while each attempt still moves the file forward.
  size_t attemptStart = 0;
  for (uint8_t resumes = 0; outcome == HttpDownloader::HTTP_ERROR && resumes < MAX_AUTO_RESUMES; ++resumes) {
    const size_t done = bytesDone.load(std::memory_order_acquire);
    if (cancelling() || done <= attemptStart || WiFi.status() != WL_CONNECTED) break;
    attemptStart = done;
    job.resume = true;
    LOG_INF("OPDS", "Auto-resume %u/%u from %zu bytes", static_cast<unsigned>(resumes + 1),
            static_cast<unsigned>(MAX_AUTO_RESUMES), done);
    outcome = HttpDownloader::downloadToFile(job.url, job.path, onProgress, nullptr, job.username, job.password,
                                             makeOptions());
  }
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
