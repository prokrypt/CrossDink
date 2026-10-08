#pragma once
#include <HalStorage.h>
#include <Stream.h>

#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace freeink {
class SecureHttpClient;
}

/**
 * HTTP client utility for fetching content and downloading files.
 * Streams requests through the configured HTTP transport so large downloads
 * do not need to fit in RAM.
 */
class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;
  using CancelCallback = std::function<bool()>;
  // Called with each body chunk as it arrives; return false to abort. Lets a
  // streaming parser consume the response without buffering the whole body.
  using DataCallback = std::function<bool(const uint8_t* data, size_t len)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
    INSUFFICIENT_SPACE,
  };

  enum class Transport {
    ESP_HTTP,
    WOLFSSL,
  };

  struct DownloadOptions {
    explicit DownloadOptions(bool preservePartial = false, bool resumePartial = false,
                             CancelCallback shouldCancel = nullptr, size_t bufferSize = 0,
                             Transport transport = Transport::ESP_HTTP)
        : preservePartial(preservePartial),
          resumePartial(resumePartial),
          shouldCancel(std::move(shouldCancel)),
          bufferSize(bufferSize),
          transport(transport) {}

    bool preservePartial;
    bool resumePartial;
    CancelCallback shouldCancel;
    size_t bufferSize;
    Transport transport;
    // Borrowed only for this synchronous request. Basic credentials are sent
    // only to this origin; empty keeps the request URL as the credential origin.
    std::string_view authorizationOrigin;
    // WOLFSSL only: send the request on this client so its kept-alive
    // connection also serves the next request to the same host (no new TCP +
    // TLS handshake). Borrowed for this synchronous call; never use one client
    // from two tasks at once. Null opens a connection per request.
    freeink::SecureHttpClient* connection = nullptr;
    // Download to "<destPath>.part" and rename it over destPath only once the
    // transfer succeeds, so a failed or interrupted download never replaces
    // (or deletes) an existing file and never leaves a truncated one behind
    // under the real name.
    bool stageAsPart = false;
    // Checks the finished file before it is accepted (and, with stageAsPart,
    // before it replaces destPath). Returning false fails the download and
    // removes the file. Catches bodies cut short without a Content-Length.
    bool (*validate)(const std::string& path) = nullptr;
    // Once the response length is known, fail with INSUFFICIENT_SPACE before
    // writing anything if the SD card cannot hold the file. The first check
    // can scan the whole FAT, so leave it off for small files.
    bool checkFreeSpace = false;
    // downloadToFile only: coalesce body chunks into a PSRAM buffer of this
    // size before each SD write (0 = write every chunk). Without PSRAM the
    // chunks are written directly.
    size_t writeBufferBytes = 0;
    // WOLFSSL only. In: sent as If-Range on a resume, so a changed file comes
    // back whole (200) instead of spliced. Out: the response's strong ETag,
    // else its Last-Modified, else empty. Borrowed for this call.
    std::string* validator = nullptr;
    // WOLFSSL only: once the body has started, fail with HTTP_ERROR after this
    // long with no new bytes (0 = the 60 s request timeout). A server that
    // drops a long response without closing otherwise costs the full timeout.
    uint32_t stallTimeoutMs = 0;
    // WOLFSSL only: fail with HTTP_ERROR when no body byte has arrived this
    // long after the request started (connect, TLS, headers; 0 = the 60 s
    // request timeout). A dead kept-alive socket otherwise costs the full
    // timeout with the radio held awake.
    uint32_t firstByteTimeoutMs = 0;
    // WOLFSSL only: send HEAD instead of GET. No body is read; the progress
    // callback reports (0, Content-Length) once, when the response is 200.
    bool headOnly = false;
  };

  /**
   * Fetch text content from a URL with optional credentials.
   */
  static bool fetchUrl(const std::string& url, std::string& outContent, const std::string& username = "",
                       const std::string& password = "");

  static bool fetchUrl(const std::string& url, Stream& stream, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream the response body to onData as it arrives, without buffering it.
   */
  static bool fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username = "",
                       const std::string& password = "");

  /**
   * Stream a URL with cancellation/progress support and a detailed result.
   */
  static DownloadError streamUrl(const std::string& url, const DataCallback& onData,
                                 ProgressCallback progress = nullptr, const std::string& username = "",
                                 const std::string& password = "", DownloadOptions options = DownloadOptions());

  /**
   * Download a file to the SD card with optional credentials.
   */
  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr, bool* cancelFlag = nullptr,
                                      const std::string& username = "", const std::string& password = "",
                                      DownloadOptions options = DownloadOptions());
};
