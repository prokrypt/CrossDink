#include "OpdsPagePrefetcher.h"

// Prefetch needs PSRAM, which the native simulator never reports.
#ifndef SIMULATOR

#include <Logging.h>
#include <TaskCores.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <utility>

#include "network/HttpDownloader.h"

namespace {
// wolfSSL handshake plus the HTTP client need more than the 4 KB used by the
// SD-bound workers; the stack is internal RAM and lives only while a job runs.
constexpr uint32_t PREFETCH_STACK_BYTES = 12 * 1024;
// Same priority as the loop and render tasks, but pinned to the worker core
// (also the Wi-Fi core) so redraws never wait on the download.
constexpr UBaseType_t PREFETCH_PRIORITY = 1;
constexpr TickType_t JOIN_POLL_TICKS = pdMS_TO_TICKS(10);
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
  cancelRequested.store(false, std::memory_order_release);
  active.store(true, std::memory_order_release);

  // The task stack must stay in internal RAM: Wi-Fi/TLS code runs on it and
  // PSRAM stacks are not safe while flash cache is disabled.
  if (xTaskCreatePinnedToCore(&taskEntry, "OpdsPrefetch", PREFETCH_STACK_BYTES, this, PREFETCH_PRIORITY, nullptr,
                              TaskCores::kWorker) != pdPASS) {
    active.store(false, std::memory_order_release);
    page.reset();
    LOG_ERR("OPDS", "Prefetch task could not start");
    return false;
  }
  return true;
}

void OpdsPagePrefetcher::join() const {
  while (running()) vTaskDelay(JOIN_POLL_TICKS);
}

void OpdsPagePrefetcher::harvestInto(OpdsPageCache& cache) {
  if (running()) return;
  if (succeeded && !page.empty()) {
    LOG_DBG("OPDS", "Caching prefetched page (%zu bytes)", page.size());
    cache.store(job.url, std::move(page));
  }
  succeeded = false;
  page.reset();
}

void OpdsPagePrefetcher::taskEntry(void* context) {
  auto* self = static_cast<OpdsPagePrefetcher*>(context);
  self->run();
  // Last touch of `self`: after this store the owner may destroy it.
  self->active.store(false, std::memory_order_release);
  vTaskDelete(nullptr);
}

void OpdsPagePrefetcher::run() {
  LOG_DBG("OPDS", "Prefetching: %s", job.url.c_str());
  HttpDownloader::DownloadOptions options;
  options.transport = HttpDownloader::Transport::WOLFSSL;
  options.authorizationOrigin = job.authorizationOrigin;
  options.shouldCancel = [this]() { return cancelRequested.load(std::memory_order_acquire); };

  const auto result = HttpDownloader::streamUrl(
      job.url,
      [this](const uint8_t* data, const size_t len) {
        return !cancelRequested.load(std::memory_order_acquire) && page.append(data, len);
      },
      nullptr, job.username, job.password, std::move(options));

  // OK means the whole body arrived; a cancel that lands after that keeps it.
  succeeded = result == HttpDownloader::OK && !page.failed() && !page.empty();
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
void OpdsPagePrefetcher::join() const {}
void OpdsPagePrefetcher::harvestInto(OpdsPageCache&) {}
void OpdsPagePrefetcher::taskEntry(void*) {}
void OpdsPagePrefetcher::run() {}

#endif  // SIMULATOR
