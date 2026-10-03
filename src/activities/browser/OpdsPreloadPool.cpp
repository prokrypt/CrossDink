#include "OpdsPreloadPool.h"

#include <Arduino.h>
#include <Knobs.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>

#include <algorithm>
#include <utility>

#include "util/UrlUtils.h"

namespace {
// Worker stacks are PSRAM (internal fallback when PSRAM is short, hence the
// block check); each worker still costs an internal TCB and wolfSSL's small
// allocations (larger ones go to PSRAM via CONFIG_SPIRAM_USE_MALLOC). Each
// further worker raises the floor, so parallelism backs off as the heap shrinks.
KNOB_ALIAS(PRELOAD_MIN_INTERNAL_FREE, opdsPreloadMinFree);  // Goodies > Knobs, as the two below
KNOB_ALIAS(PRELOAD_INTERNAL_PER_WORKER, opdsPreloadPerWorker);
KNOB_ALIAS(PRELOAD_MIN_INTERNAL_BLOCK, opdsPreloadMinBlock);
// Same rule as the foreground feed connection: a socket idle this long may
// have been dropped silently by a NAT or the server.
KNOB_ALIAS(PRELOAD_KEEPALIVE_MAX_IDLE_MS, opdsKeepaliveMs);  // Goodies > Knobs
// A page fetched this recently is not rechecked when shown from the cache.
constexpr uint32_t RECHECK_MIN_AGE_MS = 60 * 1000;
// After a network failure, start nothing for this long (the queue waits).
constexpr uint32_t FAILURE_BACKOFF_MS = 30 * 1000;
}  // namespace

OpdsPreloadPool::OpdsPreloadPool(OpdsPageCache& cache, const size_t pageMaxBytes, std::string username,
                                 std::string password, std::string authorizationOrigin)
    : cache(cache),
      pageMaxBytes(pageMaxBytes),
      username(std::move(username)),
      password(std::move(password)),
      authorizationOrigin(std::move(authorizationOrigin)) {
  queue.reserve(16);
}

// Worker destructors cancel and join each task before its connection goes.
OpdsPreloadPool::~OpdsPreloadPool() {
  for (auto& worker : workers) worker.prefetcher.cancel();
}

bool OpdsPreloadPool::queued(const std::string& url) const {
  return std::any_of(queue.begin(), queue.end(), [&url](const QueuedPage& page) { return page.url == url; });
}

bool OpdsPreloadPool::running(const std::string& url) const {
  return std::any_of(std::begin(workers), std::end(workers), [&url](const Worker& worker) {
    return worker.prefetcher.running() && worker.prefetcher.url() == url;
  });
}

size_t OpdsPreloadPool::runningCount() const {
  return static_cast<size_t>(std::count_if(std::begin(workers), std::end(workers),
                                           [](const Worker& worker) { return worker.prefetcher.running(); }));
}

bool OpdsPreloadPool::backingOff() const { return failedAtMs != 0 && millis() - failedAtMs < FAILURE_BACKOFF_MS; }

// A queue held by the failure backoff does not count: the radio may idle.
bool OpdsPreloadPool::busy() const { return (!queue.empty() && !backingOff()) || runningCount() > 0; }

void OpdsPreloadPool::enqueue(const std::string& url, const bool front) {
  if (url.empty() || cache.contains(url) || running(url)) return;
  auto existing = std::find_if(queue.begin(), queue.end(), [&url](const QueuedPage& page) { return page.url == url; });
  if (existing != queue.end()) {
    if (!front) return;
    queue.erase(existing);
  }
  if (front) {
    queue.insert(queue.begin(), QueuedPage{url, true});
  } else {
    queue.push_back(QueuedPage{url, false});
  }
}

void OpdsPreloadPool::revalidate(const std::string& url) {
  queue.erase(std::remove_if(queue.begin(), queue.end(), [](const QueuedPage& page) { return page.revalidate; }),
              queue.end());
  for (auto& worker : workers) {
    if (worker.revalidate && worker.prefetcher.running() && worker.prefetcher.url() != url) worker.prefetcher.cancel();
  }
  if (!cache.contains(url) || running(url) || cache.fetchedWithin(url, millis(), RECHECK_MIN_AGE_MS)) return;
  queue.erase(std::remove_if(queue.begin(), queue.end(), [&url](const QueuedPage& page) { return page.url == url; }),
              queue.end());
  queue.insert(queue.begin(), QueuedPage{url, true, true});
}

bool OpdsPreloadPool::takeChange(std::string& url) {
  if (changedUrl.empty()) return false;
  url = std::move(changedUrl);
  changedUrl.clear();
  return true;
}

void OpdsPreloadPool::harvest(Worker& worker) {
  if (worker.prefetcher.takeFailure()) {
    failedAtMs = millis();
    LOG_INF("OPDS", "Preload backing off %lu s after a failure", static_cast<unsigned long>(FAILURE_BACKOFF_MS / 1000));
  }
  if (worker.prefetcher.harvestInto(cache, worker.evict, worker.revalidate) && worker.revalidate) {
    changedUrl = worker.prefetcher.url();
  }
}

bool OpdsPreloadPool::startNext(Worker& worker) {
  while (!queue.empty() &&
         ((!queue.front().revalidate && cache.contains(queue.front().url)) || running(queue.front().url))) {
    queue.erase(queue.begin());
  }
  if (queue.empty() || backingOff()) return false;

  const size_t alreadyRunning = runningCount();
  const ByteHeapSnapshot internal = byteHeapSnapshot(MemoryPool::Internal);
  const size_t floor = PRELOAD_MIN_INTERNAL_FREE + alreadyRunning * PRELOAD_INTERNAL_PER_WORKER;
  if (internal.free < floor || internal.largest < PRELOAD_MIN_INTERNAL_BLOCK) {
    if (!heapLimited) {
      LOG_INF("OPDS", "Preload held: running=%zu internal free=%zu largest=%zu (floor %zu)", alreadyRunning,
              internal.free, internal.largest, floor);
      heapLimited = true;
    }
    return false;
  }
  heapLimited = false;

  QueuedPage next = std::move(queue.front());
  queue.erase(queue.begin());

  OpdsPagePrefetcher::Request request;
  request.url = next.url;
  request.username = username;
  request.password = password;
  request.authorizationOrigin = authorizationOrigin;
#if defined(FREEINK_NET_WOLFSSL)
  const unsigned long now = millis();
  if (worker.connection && now - worker.lastUseMs > PRELOAD_KEEPALIVE_MAX_IDLE_MS) worker.connection->end();
  // Small object (no buffers until it connects); without it the request just
  // opens and closes its own connection.
  if (!worker.connection) worker.connection = makeUniqueNoThrow<freeink::SecureHttpClient>();
  worker.lastUseMs = now;
  request.connection = worker.connection.get();
#endif
  worker.evict = next.evict;
  worker.revalidate = next.revalidate;
  const size_t slot = static_cast<size_t>(&worker - workers);
  if (!worker.prefetcher.start(std::move(request), pageMaxBytes)) {
    queue.insert(queue.begin(), std::move(next));
    return false;
  }
  active = true;
  LOG_INF("OPDS", "%s start: slot=%zu running=%zu queued=%zu internal free=%zu largest=%zu %s",
          next.revalidate ? "Recheck" : "Preload", slot, alreadyRunning + 1, queue.size(), internal.free,
          internal.largest, UrlUtils::maskUserInfo(next.url).c_str());
  return true;
}

void OpdsPreloadPool::collect() {
  for (auto& worker : workers) {
    if (!worker.prefetcher.running()) harvest(worker);
  }
}

void OpdsPreloadPool::pump() {
  collect();
  for (auto& worker : workers) {
    if (!worker.prefetcher.running() && !startNext(worker)) break;
  }
  if (active && !busy()) {
    active = false;
    LOG_INF("OPDS", "Preload idle: cache %zu pages %zu bytes", cache.pageCount(), cache.bytesUsed());
    closeConnections();
  }
}

bool OpdsPreloadPool::pause(const std::string& keepUrl) {
  bool waited = false;
  std::vector<QueuedPage> cancelled;
  for (auto& worker : workers) {
    if (!worker.prefetcher.running()) continue;
    if (worker.prefetcher.url() == keepUrl) {
      waited = true;
    } else {
      cancelled.push_back(QueuedPage{worker.prefetcher.url(), worker.evict});
      worker.prefetcher.cancel();
    }
  }
  for (auto& worker : workers) {
    worker.prefetcher.join();
    harvest(worker);
  }
  queue.erase(std::remove_if(queue.begin(), queue.end(), [](const QueuedPage& page) { return page.revalidate; }),
              queue.end());
  if (!cancelled.empty()) LOG_INF("OPDS", "Preload paused: %zu cancelled", cancelled.size());
  // Resume them first once the foreground request is done.
  for (auto it = cancelled.rbegin(); it != cancelled.rend(); ++it) {
    if (!cache.contains(it->url) && !queued(it->url)) queue.insert(queue.begin(), std::move(*it));
  }
  return waited;
}

void OpdsPreloadPool::closeConnections() {
#if defined(FREEINK_NET_WOLFSSL)
  for (auto& worker : workers) {
    if (worker.connection && !worker.prefetcher.running()) worker.connection->end();
  }
#endif
}
