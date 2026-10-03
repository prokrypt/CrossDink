#include "HttpDownloader.h"

#include <Arduino.h>
#include <Knobs.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <base64.h>
#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
#endif
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "AppVersion.h"
#include "TaskCores.h"
#include "network/HttpRedirectPolicy.h"
#include "network/SdWriteBehind.h"
#include "network/WifiPowerSaveGuard.h"
#include "util/UrlUtils.h"

namespace {
constexpr size_t PROGRESS_UPDATE_BYTES = 64 * 1024;
constexpr uint32_t PROGRESS_UPDATE_MS = 250;
constexpr int HTTP_RX_BUF = 4096;
constexpr int HTTP_TX_BUF = 1024;
KNOB_ALIAS(HTTP_TIMEOUT_MS, httpTimeoutMs);  // Goodies > Knobs, as the two below
KNOB_ALIAS(HTTP_READ_POLL_TIMEOUT_MS, httpReadPollMs);
KNOB_ALIAS(DOWNLOAD_IDLE_TIMEOUT_MS, downloadIdleMs);
constexpr size_t DEFAULT_DOWNLOAD_BUFFER_SIZE = 2048;
constexpr uint8_t MAX_REDIRECTS = 5;

// For logs: masked userinfo, and no query string (it can hold a signed download token).
std::string logUrl(const std::string& url) {
  std::string out = UrlUtils::maskUserInfo(url);
  out.resize(std::min(out.find('?'), out.size()));
  return out;
}

void logNetworkState(const char* phase) {
  LOG_DBG("HTTP", "%s: heap free=%" PRIu32 " maxAlloc=%" PRIu32 " wifi=%d rssi=%d", phase, ESP.getFreeHeap(),
          ESP.getMaxAllocHeap(), static_cast<int>(WiFi.status()), WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
}

void logDownloadState(const char* phase, const size_t downloaded, const size_t total, const uint32_t idleMs) {
  LOG_ERR("HTTP", "%s after %zu/%zu bytes (idle=%lu ms, timeout=%lu ms)", phase, downloaded, total,
          static_cast<unsigned long>(idleMs), static_cast<unsigned long>(DOWNLOAD_IDLE_TIMEOUT_MS));
  logNetworkState(phase);
}

bool isRedirect(const int status) {
  return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

esp_err_t captureLocationHeader(esp_http_client_event_t* evt) {
  auto* location = static_cast<std::string*>(evt->user_data);
  if (evt->event_id == HTTP_EVENT_ON_HEADER && location != nullptr && evt->header_key != nullptr &&
      evt->header_value != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
    location->assign(evt->header_value);
  }
  return ESP_OK;
}

bool isCancelRequested(bool* cancelFlag, const HttpDownloader::CancelCallback& shouldCancel) {
  if (cancelFlag && *cancelFlag) return true;
  if (shouldCancel && shouldCancel()) {
    if (cancelFlag) *cancelFlag = true;
    return true;
  }
  return false;
}

class ProgressNotifier {
 public:
  explicit ProgressNotifier(const HttpDownloader::ProgressCallback& progress) : progress_(&progress) {}

  void setTotal(const size_t total) { total_ = total; }

  void notify(size_t downloaded, bool force) {
    // Known gap: with no Content-Length (chunked or close-delimited bodies)
    // total_ stays 0 and callers never hear about progress, so the OPDS
    // download screen sits at 0% until the transfer ends. Cancel still works
    // through shouldCancel. If a server ever does this for books, report
    // bytes on a timer with total 0 and draw an indeterminate bar.
    if (!progress_ || !*progress_ || total_ == 0) return;

    const uint32_t now = millis();
    if (force || downloaded == total_ || downloaded - lastProgressBytes_ >= PROGRESS_UPDATE_BYTES ||
        now - lastProgressMs_ >= PROGRESS_UPDATE_MS) {
      lastProgressBytes_ = downloaded;
      lastProgressMs_ = now;
      (*progress_)(downloaded, total_);
    }
  }

 private:
  size_t total_ = 0;
  size_t lastProgressBytes_ = 0;
  uint32_t lastProgressMs_ = 0;
  const HttpDownloader::ProgressCallback* progress_ = nullptr;
};

struct Sink {
  std::function<bool(const uint8_t*, size_t)> write;
  HttpDownloader::ProgressCallback progress;
  bool* cancelFlag = nullptr;
  HttpDownloader::CancelCallback shouldCancel;
  size_t resumeOffset = 0;
  size_t downloaded = 0;
  size_t total = 0;
  bool rangeIgnored = false;
  bool headersChecked = false;       // wolfSSL path: first body chunk seen
  std::string* validator = nullptr;  // DownloadOptions::validator
  uint32_t lastDataMs = 0;           // millis() of the last body chunk (wolfSSL path)
  uint32_t maxWriteMs = 0;           // slowest SD write (downloadToFile)
  uint32_t stallTimeoutMs = 0;       // DownloadOptions::stallTimeoutMs
  bool stalled = false;              // the body stopped for stallTimeoutMs
  bool headOnly = false;             // DownloadOptions::headOnly
  uint32_t firstByteTimeoutMs = 0;   // DownloadOptions::firstByteTimeoutMs
  uint32_t startMs = 0;              // millis() when runGet started
};

// wolfSSL path abort poll: a user cancel, or a body that stopped arriving.
bool shouldAbortTransfer(Sink& sink) {
  if (isCancelRequested(sink.cancelFlag, sink.shouldCancel)) return true;
  if (sink.lastDataMs == 0) {
    if (sink.firstByteTimeoutMs == 0 || millis() - sink.startMs < sink.firstByteTimeoutMs) return false;
  } else if (sink.stallTimeoutMs == 0 || millis() - sink.lastDataMs < sink.stallTimeoutMs) {
    return false;
  }
  sink.stalled = true;
  return true;
}

// Tells a stalled server (idle near the timeout), a dropped link (wifi != 3)
// and a slow card (large maxWrite) apart.
[[maybe_unused]] void logStallDiagnostics(const char* what, const Sink& sink) {
  const bool connected = WiFi.status() == WL_CONNECTED;
  LOG_ERR("HTTP", "%s: got %zu of %zu bytes idle=%lu maxWrite=%lu wifi=%d rssi=%d heap=%" PRIu32 " maxAlloc=%" PRIu32,
          what, sink.downloaded, sink.total,
          sink.lastDataMs != 0 ? static_cast<unsigned long>(millis() - sink.lastDataMs) : 0UL,
          static_cast<unsigned long>(sink.maxWriteMs), static_cast<int>(WiFi.status()), connected ? WiFi.RSSI() : 0,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void setRequestHeaders(esp_http_client_handle_t client, const std::string& username, const std::string& password,
                       size_t resumeOffset, bool sendAuthorization) {
  esp_http_client_set_header(client, "User-Agent", AppVersion::userAgent());
  esp_http_client_set_header(client, "Connection", "close");
  if (resumeOffset > 0) {
    char rangeHeader[40];
    snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", resumeOffset);
    esp_http_client_set_header(client, "Range", rangeHeader);
    LOG_DBG("HTTP", "Resuming download at byte %zu", resumeOffset);
  }
  if (sendAuthorization) {
    const std::string credentials = username + ":" + password;
    const String header = "Basic " + base64::encode(credentials.c_str());
    esp_http_client_set_header(client, "Authorization", header.c_str());
  }
}

void logTlsError(esp_http_client_handle_t client, const char* phase) {
  int tlsError = 0;
  int tlsFlags = 0;
  const esp_err_t err = esp_http_client_get_and_clear_last_tls_error(client, &tlsError, &tlsFlags);
  if (err != ESP_OK || tlsError != 0 || tlsFlags != 0) {
    const int tlsCode = tlsError < 0 ? -tlsError : tlsError;
    LOG_ERR("HTTP", "%s TLS error: err=%s mbedtls=0x%x flags=0x%x", phase, esp_err_to_name(err), tlsCode, tlsFlags);
  }
}

#if defined(FREEINK_NET_WOLFSSL)
HttpDownloader::DownloadError runGetWolfSsl(const std::string& url, const std::string& username,
                                            const std::string& password,
                                            const HttpRedirectPolicy::Url& credentialOrigin, const bool hasCredentials,
                                            Sink& sink, const size_t bufferSize,
                                            freeink::SecureHttpClient* const sharedHttp) {
  (void)bufferSize;  // SecureHttpClient owns one fixed 1024-byte streaming buffer.
  std::string currentUrl = url;
  // A caller-owned client keeps its connection open for the caller's next
  // request; a local one closes when this function returns.
  std::optional<freeink::SecureHttpClient> localHttp;
  if (!sharedHttp) localHttp.emplace();
  freeink::SecureHttpClient& http = sharedHttp ? *sharedHttp : *localHttp;
  ProgressNotifier progressNotifier(sink.progress);

  for (uint8_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
    HttpRedirectPolicy::Url currentOrigin;
    const bool currentParsed = HttpRedirectPolicy::parseUrl(currentUrl, currentOrigin);
    const bool sendAuthorization =
        currentParsed && HttpRedirectPolicy::shouldSendAuthorization(currentOrigin, credentialOrigin, hasCredentials);

    http.setTimeout(HTTP_TIMEOUT_MS);
    // SecureNet does not yet expose ESP-IDF's CA bundle. This matches the
    // existing KOSync transport; cross-origin hops omit Basic credentials.
    http.setInsecure();
    if (!http.begin(currentUrl)) {
      LOG_ERR("HTTP", "wolfSSL rejected URL: %s", logUrl(currentUrl).c_str());
      return HttpDownloader::HTTP_ERROR;
    }
    // Replace SecureHttpClient's built-in User-Agent so strict servers receive
    // exactly one header while retaining CrossDink's device/version identity.
    http.setUserAgent(AppVersion::userAgent());
    if (sink.resumeOffset > 0) {
      char rangeHeader[40];
      snprintf(rangeHeader, sizeof(rangeHeader), "bytes=%zu-", sink.resumeOffset);
      http.addHeader("Range", rangeHeader);
      const bool ifRange = sink.validator && !sink.validator->empty();
      if (ifRange) http.addHeader("If-Range", *sink.validator);
      LOG_INF("HTTP", "Resume request from byte %zu (If-Range=%s)", sink.resumeOffset,
              ifRange ? sink.validator->c_str() : "none");
    }
    if (sendAuthorization) {
      const std::string credentials = username + ":" + password;
      const String encoded = base64::encode(credentials.c_str());
      http.addHeader("Authorization", std::string("Basic ") + encoded.c_str());
    }

    // "shared": a following request to the same host logs no new TLS handshake.
    const char* method = sink.headOnly ? "HEAD" : "GET";
    LOG_DBG("HTTP", "wolfSSL %s%s: %s", method, sharedHttp ? " (shared)" : "", logUrl(currentUrl).c_str());
    const int status = http.sendRequest(
        method, nullptr, 0,
        [&http, &sink, &progressNotifier](const uint8_t* data, const size_t len) {
          const int responseStatus = http.getStatus();
          const bool isResumeResponse = sink.resumeOffset > 0 && responseStatus == 206;
          if (responseStatus != 200 && !isResumeResponse) return true;
          if (sink.resumeOffset > 0 && !isResumeResponse) {
            sink.rangeIgnored = true;
            return false;
          }
          if (!sink.headersChecked) {
            sink.headersChecked = true;
            if (isResumeResponse) {
              // "bytes <start>-<end>/<total>": the body must continue exactly
              // where the partial file ends.
              const std::string range = http.getHeader("content-range");
              char* end = nullptr;
              const unsigned long long start =
                  range.rfind("bytes ", 0) == 0 ? strtoull(range.c_str() + 6, &end, 10) : 0;
              if (!end || *end != '-' || start != sink.resumeOffset) {
                LOG_INF("HTTP", "Content-Range '%s' does not match byte %zu", range.c_str(), sink.resumeOffset);
                sink.rangeIgnored = true;
                return false;
              }
              LOG_INF("HTTP", "Resume accepted: 206 from byte %zu", sink.resumeOffset);
            }
            if (sink.validator) {
              // If-Range needs a strong validator: skip weak ETags.
              const std::string etag = http.getHeader("etag");
              *sink.validator = !etag.empty() && etag.rfind("W/", 0) != 0 ? etag : http.getHeader("last-modified");
            }
          }

          if (sink.downloaded < sink.resumeOffset) sink.downloaded = sink.resumeOffset;
          if (sink.total == 0 && http.hasContentLength()) {
            sink.total = sink.resumeOffset + http.getContentLength();
            progressNotifier.setTotal(sink.total);
          }
          if (!sink.write(data, len)) return false;
          sink.downloaded += len;
          sink.lastDataMs = millis();
          progressNotifier.notify(sink.downloaded, false);
          return true;
        },
        [&sink]() { return shouldAbortTransfer(sink); });

    if (sink.stalled) {
      logStallDiagnostics("Stalled", sink);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.aborted()) return HttpDownloader::ABORTED;
    // 416: the partial file is not a prefix the server can continue.
    if (sink.resumeOffset > 0 && status == 416) sink.rangeIgnored = true;
    if (sink.rangeIgnored) {
      LOG_INF("HTTP", "Server ignored range request (status %d); restarting download", status);
      sink.resumeOffset = 0;
      return HttpDownloader::HTTP_ERROR;
    }
    if (status < 0) {
      LOG_ERR("HTTP", "wolfSSL request failed: %s", logUrl(currentUrl).c_str());
      if (sink.downloaded > 0) logStallDiagnostics("Request failed", sink);
      logNetworkState("wolfSSL request failure");
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      const std::string location = http.getHeader("location");
      if (location.empty()) {
        LOG_ERR("HTTP", "Redirect missing Location header");
        return HttpDownloader::HTTP_ERROR;
      }

      const std::string redirectUrl = HttpRedirectPolicy::buildRedirectUrl(currentUrl, location);
      HttpRedirectPolicy::Url redirect;
      if (!HttpRedirectPolicy::parseUrl(redirectUrl, redirect)) {
        LOG_ERR("HTTP", "Rejected redirect with unsupported Location");
        return HttpDownloader::HTTP_ERROR;
      }
      if (currentParsed && !HttpRedirectPolicy::isAllowedRedirect(currentOrigin, redirect)) {
        LOG_ERR("HTTP", "Rejected HTTPS downgrade redirect to %s", UrlUtils::maskUserInfo(redirect.host).c_str());
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = redirectUrl;
      LOG_DBG("HTTP", "Redirecting to: %s", UrlUtils::maskUserInfo(redirect.host).c_str());
      continue;
    }

    const bool isResumeResponse = sink.resumeOffset > 0 && status == 206;
    if (status != 200 && !isResumeResponse) {
      LOG_ERR("HTTP", "Unexpected status: %d", status);
      return HttpDownloader::HTTP_ERROR;
    }
    if (http.callbackAborted()) {
      LOG_ERR("HTTP", "Write failed after %zu/%zu bytes", sink.downloaded, sink.total);
      return HttpDownloader::FILE_ERROR;
    }
    if (!http.responseComplete()) {
      logStallDiagnostics("Incomplete", sink);
      return HttpDownloader::HTTP_ERROR;
    }

    if (sink.total == 0 && http.hasContentLength()) {
      sink.total = sink.resumeOffset + http.getContentLength();
      progressNotifier.setTotal(sink.total);
    }
    progressNotifier.notify(sink.downloaded, true);
    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "Redirect limit exceeded");
  return HttpDownloader::HTTP_ERROR;
}
#endif

HttpDownloader::DownloadError runGetDefault(const std::string& url, const std::string& username,
                                            const std::string& password,
                                            const HttpRedirectPolicy::Url& credentialOrigin, const bool hasCredentials,
                                            Sink& sink, const size_t bufferSize) {
  std::string currentUrl = url;

  for (uint8_t hop = 0; hop < MAX_REDIRECTS; ++hop) {
    HttpRedirectPolicy::Url currentOrigin;
    const bool currentParsed = HttpRedirectPolicy::parseUrl(currentUrl, currentOrigin);
    const bool sendAuthorization =
        currentParsed && HttpRedirectPolicy::shouldSendAuthorization(currentOrigin, credentialOrigin, hasCredentials);
    std::string redirectLocation;

    esp_http_client_config_t config = {};
    config.url = currentUrl.c_str();
    config.buffer_size = HTTP_RX_BUF;
    config.buffer_size_tx = HTTP_TX_BUF;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.keep_alive_enable = false;
    config.event_handler = captureLocationHeader;
    config.user_data = &redirectLocation;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
      LOG_ERR("HTTP", "Client init failed");
      logNetworkState("Client init failure");
      return HttpDownloader::HTTP_ERROR;
    }

    setRequestHeaders(client, username, password, sink.resumeOffset, sendAuthorization);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Open failed: %s", esp_err_to_name(err));
      logTlsError(client, "Open failure");
      logNetworkState("Open failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    int64_t responseLength = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (responseLength < 0) {
      LOG_ERR("HTTP", "Fetch headers failed: %lld", static_cast<long long>(responseLength));
      logNetworkState("Fetch headers failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    if (isRedirect(status)) {
      if (redirectLocation.empty()) {
        LOG_ERR("HTTP", "Redirect missing Location header");
        logNetworkState("Redirect missing Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }

      const std::string redirectUrl = HttpRedirectPolicy::buildRedirectUrl(currentUrl, redirectLocation);
      HttpRedirectPolicy::Url redirect;
      if (!HttpRedirectPolicy::parseUrl(redirectUrl, redirect)) {
        LOG_ERR("HTTP", "Rejected redirect with unsupported Location");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (currentParsed && !HttpRedirectPolicy::isAllowedRedirect(currentOrigin, redirect)) {
        LOG_ERR("HTTP", "Rejected HTTPS downgrade redirect to %s", UrlUtils::maskUserInfo(redirect.host).c_str());
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      currentUrl = redirectUrl;
      LOG_DBG("HTTP", "Redirecting to: %s", UrlUtils::maskUserInfo(redirect.host).c_str());
      esp_http_client_cleanup(client);
      continue;
    }

    const bool isResumeResponse = sink.resumeOffset > 0 && status == 206;
    if (status != 200 && !isResumeResponse) {
      LOG_ERR("HTTP", "Unexpected status: %d", status);
      logNetworkState("Unexpected status");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
    if (sink.resumeOffset > 0 && !isResumeResponse) {
      LOG_DBG("HTTP", "Server ignored range request; restarting download");
      sink.rangeIgnored = true;
      sink.resumeOffset = 0;
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    const size_t bodyLength = responseLength > 0 ? static_cast<size_t>(responseLength) : 0;
    sink.total = bodyLength > 0 ? sink.resumeOffset + bodyLength : 0;
    sink.downloaded = sink.resumeOffset;
    if (sink.total > 0) {
    } else {
    }
#ifdef ESP_ERR_HTTP_EAGAIN
    err = esp_http_client_set_timeout_ms(client, HTTP_READ_POLL_TIMEOUT_MS);
    if (err != ESP_OK) {
      LOG_ERR("HTTP", "Failed to set read timeout: %s", esp_err_to_name(err));
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }
#endif

    auto buffer = makeUniqueNoThrow<char[]>(bufferSize);
    if (!buffer) {
      LOG_ERR("HTTP", "Failed to allocate %zu byte download buffer", bufferSize);
      logNetworkState("Download buffer allocation failure");
      esp_http_client_cleanup(client);
      return HttpDownloader::HTTP_ERROR;
    }

    ProgressNotifier progressNotifier(sink.progress);
    progressNotifier.setTotal(sink.total);
#ifdef ESP_ERR_HTTP_EAGAIN
    uint32_t lastReadMs = millis();
#endif
    while (true) {
      if (isCancelRequested(sink.cancelFlag, sink.shouldCancel)) {
        esp_http_client_cleanup(client);
        return HttpDownloader::ABORTED;
      }

      const int bytesRead = esp_http_client_read(client, buffer.get(), bufferSize);
      if (bytesRead < 0) {
#ifdef ESP_ERR_HTTP_EAGAIN
        if (bytesRead == -ESP_ERR_HTTP_EAGAIN) {
          const uint32_t idleMs = millis() - lastReadMs;
          if (idleMs >= DOWNLOAD_IDLE_TIMEOUT_MS) {
            logDownloadState("Read timed out", sink.downloaded, sink.total, idleMs);
            esp_http_client_cleanup(client);
            return HttpDownloader::HTTP_ERROR;
          }
          delay(1);
          continue;
        }
#endif
        LOG_ERR("HTTP", "Read error after %zu/%zu bytes", sink.downloaded, sink.total);
        logNetworkState("Read error");
        esp_http_client_cleanup(client);
        return HttpDownloader::HTTP_ERROR;
      }
      if (bytesRead == 0) break;

      if (!sink.write(reinterpret_cast<const uint8_t*>(buffer.get()), static_cast<size_t>(bytesRead))) {
        LOG_ERR("HTTP", "Write failed after %zu/%zu bytes", sink.downloaded, sink.total);
        logNetworkState("Write failure");
        esp_http_client_cleanup(client);
        return HttpDownloader::FILE_ERROR;
      }

      sink.downloaded += static_cast<size_t>(bytesRead);
#ifdef ESP_ERR_HTTP_EAGAIN
      lastReadMs = millis();
#endif
      if (sink.total > 0 && sink.total <= PROGRESS_UPDATE_BYTES) {
      }
      progressNotifier.notify(sink.downloaded, false);
      if (sink.total > 0 && sink.downloaded >= sink.total) break;
      delay(0);
    }

    const bool complete = esp_http_client_is_complete_data_received(client);
    esp_http_client_cleanup(client);
    progressNotifier.notify(sink.downloaded, true);
    if (!complete) {
      LOG_ERR("HTTP", "Incomplete: got %zu of %zu bytes", sink.downloaded, sink.total);
      logNetworkState("Incomplete transfer");
      return HttpDownloader::HTTP_ERROR;
    }

    return HttpDownloader::OK;
  }

  LOG_ERR("HTTP", "Redirect limit exceeded");
  logNetworkState("Redirect limit exceeded");
  return HttpDownloader::HTTP_ERROR;
}

HttpDownloader::DownloadError runGetTransport(const std::string& url, const std::string& username,
                                              const std::string& password, const std::string_view authorizationOrigin,
                                              Sink& sink, const size_t bufferSize,
                                              const HttpDownloader::Transport transport,
                                              freeink::SecureHttpClient* const sharedHttp) {
  HttpRedirectPolicy::Url credentialOrigin;
  const std::string_view credentialUrl = authorizationOrigin.empty() ? std::string_view(url) : authorizationOrigin;
  const bool hasCredentials =
      !username.empty() && !password.empty() && HttpRedirectPolicy::parseUrl(credentialUrl, credentialOrigin);
#if defined(FREEINK_NET_WOLFSSL)
  if (transport == HttpDownloader::Transport::WOLFSSL) {
    return runGetWolfSsl(url, username, password, credentialOrigin, hasCredentials, sink, bufferSize, sharedHttp);
  }
#else
  (void)transport;
  (void)sharedHttp;
#endif
  return runGetDefault(url, username, password, credentialOrigin, hasCredentials, sink, bufferSize);
}

HttpDownloader::DownloadError runGet(const std::string& url, const std::string& username, const std::string& password,
                                     const std::string_view authorizationOrigin, Sink& sink, const size_t bufferSize,
                                     const HttpDownloader::Transport transport,
                                     freeink::SecureHttpClient* const sharedHttp = nullptr) {
  const unsigned long startedMs = millis();
  sink.startMs = startedMs;
  const size_t startBytes = sink.downloaded;
  const auto result =
      runGetTransport(url, username, password, authorizationOrigin, sink, bufferSize, transport, sharedHttp);
  // One line per request (redirects and TLS handshakes included) for KB/s.
  const unsigned long ms = millis() - startedMs;
  const size_t bytes = sink.downloaded - startBytes;
  LOG_DBG("HTTP", "GET done: err=%d bytes=%u ms=%lu KBps=%lu", static_cast<int>(result), static_cast<unsigned>(bytes),
          ms, ms > 0 ? static_cast<unsigned long>(bytes / ms) : 0UL);
  (void)ms;
  (void)bytes;
  return result;
}
}  // namespace

bool HttpDownloader::fetchUrl(const std::string& url, Stream& outContent, const std::string& username,
                              const std::string& password) {
  return fetchUrl(
      url, [&outContent](const uint8_t* data, size_t len) { return outContent.write(data, len) == len; }, username,
      password);
}

bool HttpDownloader::fetchUrl(const std::string& url, std::string& outContent, const std::string& username,
                              const std::string& password) {
  outContent.clear();
  return fetchUrl(
      url,
      [&outContent](const uint8_t* data, size_t len) {
        outContent.append(reinterpret_cast<const char*>(data), len);
        return true;
      },
      username, password);
}

bool HttpDownloader::fetchUrl(const std::string& url, const DataCallback& onData, const std::string& username,
                              const std::string& password) {
  return streamUrl(url, onData, nullptr, username, password) == OK;
}

HttpDownloader::DownloadError HttpDownloader::streamUrl(const std::string& url, const DataCallback& onData,
                                                        ProgressCallback progress, const std::string& username,
                                                        const std::string& password, DownloadOptions options) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  if (!onData) {
    LOG_ERR("HTTP", "Fetch failed: missing data callback");
    return HTTP_ERROR;
  }

  Sink sink;
  sink.write = onData;
  sink.progress = std::move(progress);
  sink.shouldCancel = std::move(options.shouldCancel);
  sink.headOnly = options.headOnly;
  sink.firstByteTimeoutMs = options.firstByteTimeoutMs;
  sink.stallTimeoutMs = options.stallTimeoutMs;
  const size_t bufferSize = options.bufferSize > 0 ? options.bufferSize : DEFAULT_DOWNLOAD_BUFFER_SIZE;
  return runGet(url, username, password, options.authorizationOrigin, sink, bufferSize, options.transport,
                options.connection);
}

HttpDownloader::DownloadError HttpDownloader::downloadToFile(const std::string& url, const std::string& destPath,
                                                             ProgressCallback progress, bool* cancelFlag,
                                                             const std::string& username, const std::string& password,
                                                             DownloadOptions options) {
  WifiPowerSaveGuard wifiPowerSaveGuard;
  (void)wifiPowerSaveGuard;

  const size_t bufferSize = options.bufferSize > 0 ? options.bufferSize : DEFAULT_DOWNLOAD_BUFFER_SIZE;
  const std::string writePath = options.stageAsPart ? destPath + ".part" : destPath;
  size_t resumeOffset = 0;
  if (options.resumePartial && Storage.exists(writePath.c_str())) {
    FsFile existingFile;
    if (Storage.openFileForRead("HTTP", writePath.c_str(), existingFile)) {
      resumeOffset = existingFile.fileSize();
      existingFile.close();
    }
  }

  if (resumeOffset == 0 && Storage.exists(writePath.c_str())) {
    Storage.remove(writePath.c_str());
  }

  Sink sink;
  sink.progress = std::move(progress);
  sink.cancelFlag = cancelFlag;
  sink.shouldCancel = std::move(options.shouldCancel);
  sink.resumeOffset = resumeOffset;
  sink.validator = options.validator;
  sink.stallTimeoutMs = options.stallTimeoutMs;

  FsFile file;
  bool fileOpen = false;
  bool insufficientSpace = false;
  // Free space is only measured once a write fails: counting free clusters
  // scans the whole FAT, which takes seconds on a large FAT32 card and used to
  // hold up the first byte of every download. A failed write leaves only the
  // .part file, which the failure path removes.
  auto noteWriteFailure = [&]() {
    if (!options.checkFreeSpace || insufficientSpace) return;
    // Some SD transports cannot report capacity; report a plain write failure.
    const uint64_t totalBytes = Storage.totalBytes();
    if (totalBytes == 0) return;
    const uint64_t usedBytes = Storage.usedBytes();
    const uint64_t freeBytes = totalBytes > usedBytes ? totalBytes - usedBytes : 0;
    const uint64_t neededBytes = sink.total > sink.downloaded ? sink.total - sink.downloaded : 0;
    // A nearly full card (under 1 MB left) counts as full even without a size.
    if (freeBytes < neededBytes || freeBytes < 1024 * 1024) {
      LOG_ERR("HTTP", "Insufficient SD space: free=%llu required=%llu", static_cast<unsigned long long>(freeBytes),
              static_cast<unsigned long long>(neededBytes));
      insufficientSpace = true;
    }
  };
  auto openOutputFile = [&]() {
    if (fileOpen) return true;
    // Cheap up-front check (capacity is cached at mount): a book larger than
    // the whole card can never fit.
    const uint64_t totalBytes = Storage.totalBytes();
    if (options.checkFreeSpace && totalBytes > 0 && sink.total > sink.resumeOffset &&
        sink.total - sink.resumeOffset > totalBytes) {
      LOG_ERR("HTTP", "Insufficient SD space: card=%llu required=%llu", static_cast<unsigned long long>(totalBytes),
              static_cast<unsigned long long>(sink.total - sink.resumeOffset));
      insufficientSpace = true;
      return false;
    }
    if (sink.resumeOffset > 0) {
      file = Storage.open(writePath.c_str(), O_WRONLY | O_APPEND);
    } else {
      fileOpen = Storage.openFileForWrite("HTTP", writePath.c_str(), file);
      if (!fileOpen) {
        LOG_ERR("HTTP", "Failed to open file for writing");
        return false;
      }
    }
    fileOpen = file;
    if (!fileOpen) {
      LOG_ERR("HTTP", "Failed to open file for writing");
    }
    return fileOpen;
  };

  // Fewer, larger SD writes: each write costs a FAT/cluster update, so 1-2 KB
  // TLS records written one by one spend much of the time in card overhead.
  HeapByteBuffer writeBuf;
  size_t writeBufLen = 0;
  if (options.writeBufferBytes > 0) {
    writeBuf = makePsramByteBufferNoThrow(options.writeBufferBytes);
    if (!writeBuf) LOG_DBG("HTTP", "No PSRAM write buffer; writing chunks directly");
  }
  auto timedWrite = [&](const uint8_t* data, size_t len) {
    const uint32_t startMs = millis();
    const bool ok = file.write(data, len) == len;
    const uint32_t tookMs = millis() - startMs;
    if (tookMs > sink.maxWriteMs) sink.maxWriteMs = tookMs;
    if (!ok) noteWriteFailure();
    return ok;
  };
  auto flushWriteBuf = [&]() {
    if (writeBufLen == 0) return true;
    const bool ok = timedWrite(writeBuf.get(), writeBufLen);
    writeBufLen = 0;
    return ok;
  };
  // With PSRAM, a writer task on the worker core writes one 32 KB buffer while
  // this task fills the other: the receive no longer waits on the card.
  SdWriteBehind writeBehind;
  bool writeBehindTried = false;
  auto writeChunk = [&](const uint8_t* data, size_t len) {
    if (!openOutputFile()) return false;
    if (!writeBehindTried && options.writeBufferBytes > 0) {
      writeBehindTried = true;
      writeBehind.begin([](void* ctx, const uint8_t* bytes,
                           size_t count) { return (*static_cast<decltype(timedWrite)*>(ctx))(bytes, count); },
                        &timedWrite, "HttpWriter", TaskCores::kWorker);
    }
    if (writeBehind.active()) return writeBehind.append(data, len);
    if (!writeBuf) return timedWrite(data, len);
    if (writeBufLen + len > options.writeBufferBytes && !flushWriteBuf()) return false;
    if (len >= options.writeBufferBytes) return timedWrite(data, len);
    memcpy(writeBuf.get() + writeBufLen, data, len);
    writeBufLen += len;
    return true;
  };
  sink.write = writeChunk;

  DownloadError result =
      runGet(url, username, password, options.authorizationOrigin, sink, bufferSize, options.transport);
  if (sink.rangeIgnored) {
    writeBehind.abort();  // the writer task must be done with the file first
    writeBehindTried = false;
    if (fileOpen) {
      file.close();
      fileOpen = false;
    }
    Storage.remove(writePath.c_str());
    writeBufLen = 0;
    sink.rangeIgnored = false;
    sink.headersChecked = false;
    sink.resumeOffset = 0;
    sink.downloaded = 0;
    sink.total = 0;
    sink.write = writeChunk;
    result = runGet(url, username, password, options.authorizationOrigin, sink, bufferSize, options.transport);
  }

  if (fileOpen) {
    // Also on failure: a resume continues from the file's size, so every
    // received byte should land.
    if (!writeBehind.finish() && result == OK) {
      LOG_ERR("HTTP", "Write-behind failed after %zu bytes", sink.downloaded);
      result = FILE_ERROR;
    }
    if (!flushWriteBuf() && result == OK) {
      LOG_ERR("HTTP", "Final buffered write failed after %zu bytes", sink.downloaded);
      result = FILE_ERROR;
    }
    file.flush();
    file.close();
  }
  if (insufficientSpace) result = INSUFFICIENT_SPACE;

  if (result != OK) {
    if (result == ABORTED) {
      LOG_WRN("HTTP", "Transfer cancelled: downloaded=%zu expected=%zu", sink.downloaded, sink.total);
    } else {
      LOG_ERR("HTTP", "Transfer failed: error=%d downloaded=%zu expected=%zu preservePartial=%d resumePartial=%d",
              static_cast<int>(result), sink.downloaded, sink.total, options.preservePartial, options.resumePartial);
    }
    // A full card cannot take the rest either: free the space now.
    if (result == ABORTED || result == INSUFFICIENT_SPACE || !options.preservePartial) {
      Storage.remove(writePath.c_str());
    }
    return result;
  }

  if (sink.downloaded == 0) {
    LOG_ERR("HTTP", "Download failed: no data received");
    if (!options.preservePartial) {
      Storage.remove(writePath.c_str());
    }
    return HTTP_ERROR;
  }

  if (sink.total > 0 && sink.downloaded != sink.total) {
    LOG_ERR("HTTP", "Size mismatch: got %zu, expected %zu", sink.downloaded, sink.total);
    if (!options.preservePartial) {
      Storage.remove(writePath.c_str());
    }
    return HTTP_ERROR;
  }

  if (options.validate && !options.validate(writePath)) {
    LOG_ERR("HTTP", "Downloaded file failed validation: %s", writePath.c_str());
    Storage.remove(writePath.c_str());
    return HTTP_ERROR;
  }

  if (options.stageAsPart) {
    // FAT rename will not replace an existing file, so move the old copy aside
    // first and only delete it once the new one is in place. A failed swap puts
    // the old copy back, so a failed download never costs the user their book.
    const std::string backupPath = destPath + ".old";
    const bool hadOld = Storage.exists(destPath.c_str());
    if (hadOld) {
      if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());
      if (!Storage.rename(destPath.c_str(), backupPath.c_str())) {
        LOG_ERR("HTTP", "Could not move aside %s", destPath.c_str());
        Storage.remove(writePath.c_str());
        return FILE_ERROR;
      }
    }
    if (!Storage.rename(writePath.c_str(), destPath.c_str())) {
      LOG_ERR("HTTP", "Could not rename %s to %s", writePath.c_str(), destPath.c_str());
      Storage.remove(writePath.c_str());
      if (hadOld && !Storage.rename(backupPath.c_str(), destPath.c_str())) {
        LOG_ERR("HTTP", "Could not restore %s", destPath.c_str());
      }
      return FILE_ERROR;
    }
    if (hadOld && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("HTTP", "Could not remove %s", backupPath.c_str());
    }
  }

  return OK;
}
