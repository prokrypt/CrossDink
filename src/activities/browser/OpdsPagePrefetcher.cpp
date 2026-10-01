#include "OpdsPagePrefetcher.h"

// Prefetch needs PSRAM, which the native simulator never reports.
#ifndef SIMULATOR

#include <Logging.h>

#include <utility>

#include "network/HttpDownloader.h"
#include "util/UrlUtils.h"

namespace {
// wolfSSL handshake plus the HTTP client need more than the 4 KB used by the
// SD-bound workers. The stack is PSRAM (3 workers x 12 KB of internal RAM
// fragmented the block the Wi-Fi exit needs); the job only does TLS/HTTP into
// a PSRAM buffer and never writes flash (the one thing a PSRAM stack forbids).
constexpr uint32_t PREFETCH_STACK_BYTES = 12 * 1024;
// No byte for this long (before or during the body): give up instead of the
// 60 s request timeout, so a dead socket doesn't hold the radio awake.
constexpr uint32_t PREFETCH_SILENCE_TIMEOUT_MS = 10000;
}  // namespace

OpdsPagePrefetcher::~OpdsPagePrefetcher() {
  cancel();
  join();
}

bool OpdsPagePrefetcher::start(Request&& request, const size_t maxBytes) {
  if (running()) return false;

  job = std::move(request);
  page = OpdsPageBuffer(MemoryPool::Psram, maxBytes);
  succeeded = false;
  networkFailed = false;
  cancelRequested.store(false, std::memory_order_release);
  if (!task.start([](void* self) { static_cast<OpdsPagePrefetcher*>(self)->run(); }, this, PREFETCH_STACK_BYTES,
                  "OpdsPrefetch", false, WorkerTask::Stack::Psram)) {
    page.reset();
    LOG_ERR("OPDS", "Prefetch task could not start");
    return false;
  }
  return true;
}

bool OpdsPagePrefetcher::harvestInto(OpdsPageCache& cache, const bool mayEvict, const bool onlyIfChanged) {
  if (running()) return false;
  task.join();  // not running: returns at once and frees the parked task
  bool stored = false;
  if (succeeded && !page.empty()) {
    const OpdsPageBuffer* cached = onlyIfChanged ? cache.find(job.url) : nullptr;
    if (cached && OpdsPageCache::sameFeed(*cached, page)) {
      LOG_INF("OPDS", "Recheck unchanged: %s", UrlUtils::maskUserInfo(job.url).c_str());
      cache.markFetched(job.url, millis());
    } else {
      if (onlyIfChanged) {
        LOG_INF("OPDS", "Recheck changed (%zu bytes): %s", page.size(), UrlUtils::maskUserInfo(job.url).c_str());
      } else {
        LOG_DBG("OPDS", "Caching prefetched page (%zu bytes)", page.size());
      }
      stored = cache.store(job.url, std::move(page), mayEvict, millis());
      if (!stored) LOG_DBG("OPDS", "Prefetched page not cached (full)");
    }
  }
  succeeded = false;
  page.reset();
  return stored;
}

void OpdsPagePrefetcher::run() {
  LOG_DBG("OPDS", "Prefetching: %s", UrlUtils::maskUserInfo(job.url).c_str());
  HttpDownloader::DownloadOptions options;
  options.transport = HttpDownloader::Transport::WOLFSSL;
  options.authorizationOrigin = job.authorizationOrigin;
  options.connection = job.connection;
  options.shouldCancel = [this]() { return cancelRequested.load(std::memory_order_acquire); };
  // Feed pages are small: a silent socket is dead, not slow.
  options.firstByteTimeoutMs = PREFETCH_SILENCE_TIMEOUT_MS;
  options.stallTimeoutMs = PREFETCH_SILENCE_TIMEOUT_MS;

  const auto result = HttpDownloader::streamUrl(
      job.url,
      [this](const uint8_t* data, const size_t len) {
        return !cancelRequested.load(std::memory_order_acquire) && page.append(data, len);
      },
      nullptr, job.username, job.password, std::move(options));

  // OK means the whole body arrived; a cancel that lands after that keeps it.
  succeeded = result == HttpDownloader::OK && !page.failed() && !page.empty();
  networkFailed = result == HttpDownloader::HTTP_ERROR && !cancelRequested.load(std::memory_order_acquire);
  if (!succeeded) {
    LOG_INF("OPDS", "Prefetch %s (result=%d, overflow=%d)",
            cancelRequested.load(std::memory_order_acquire) ? "cancelled" : "failed", static_cast<int>(result),
            page.failed() ? 1 : 0);
    page.reset();
  } else {
    LOG_INF("OPDS", "Prefetch done: %zu bytes", page.size());
  }
}

#else  // SIMULATOR

// The simulator reports no PSRAM, so the activity never creates a prefetcher;
// these keep the link complete.
OpdsPagePrefetcher::~OpdsPagePrefetcher() = default;
bool OpdsPagePrefetcher::start(Request&&, size_t) { return false; }
bool OpdsPagePrefetcher::harvestInto(OpdsPageCache&, bool, bool) { return false; }
void OpdsPagePrefetcher::run() {}

#endif  // SIMULATOR
