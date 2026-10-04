#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "OpdsPageCache.h"
#include "OpdsPagePrefetcher.h"

namespace freeink {
class SecureHttpClient;
}

/**
 * Small pool of OpdsPagePrefetcher workers that download queued OPDS feed
 * pages into the page cache in parallel (PSRAM devices only).
 *
 * Main loop only: enqueue(), pump(), pause() and closeConnections() must all
 * run on the task that owns the cache. Each worker keeps its own TLS
 * connection, so workers never share a client; the activity's foreground
 * connection is never handed to the pool. A worker starts only while internal
 * RAM stays above a floor that grows with each running worker, so the pool
 * shrinks to one (or zero) downloads when the heap is tight.
 */
class OpdsPreloadPool {
 public:
  static constexpr size_t MAX_WORKERS = 3;
  static constexpr uint32_t MARKS_BATCH_MS = 1000;

  OpdsPreloadPool(OpdsPageCache& cache, size_t pageMaxBytes, std::string username, std::string password,
                  std::string authorizationOrigin);
  ~OpdsPreloadPool();
  OpdsPreloadPool(const OpdsPreloadPool&) = delete;
  OpdsPreloadPool& operator=(const OpdsPreloadPool&) = delete;

  // Queues a feed URL unless it is cached, queued or running. front: fetch it
  // before the rest (the next page), and let it evict older cache pages; other
  // pages are stored only when they fit without evicting anything.
  void enqueue(const std::string& url, bool front);
  // Same, with this page's own credentials (another server's page) instead of
  // the pool's.
  void enqueue(const std::string& url, bool front, std::string username, std::string password,
               std::string authorizationOrigin);
  // Refetches a cached page ahead of the queue and replaces the cached copy if
  // the server's differs (see takeChange). Replaces any earlier recheck.
  void revalidate(const std::string& url);
  // Moves the URL of the last recheck that changed the cache into url.
  bool takeChange(std::string& url);
  // Moves finished pages into the cache.
  void collect();
  // collect(), then starts queued URLs on idle workers.
  void pump();
  // Before a foreground request: lets the job for keepUrl finish, cancels the
  // others (they go back to the front of the queue; rechecks are dropped),
  // joins every worker and caches what finished. Returns true when it waited
  // for keepUrl.
  bool pause(const std::string& keepUrl);
  // pause()'s cancel without the wait: a tap on an uncached page stops the
  // other downloads at once so they don't share Wi-Fi with it. Main loop only.
  void cancelOthers(const std::string& keepUrl);
  // Drops the workers' kept-alive connections (all workers must be idle).
  void closeConnections();
  bool busy() const;
  // Row marks for pages landed since pendingSinceMs (millis() | 1, 0: none)
  // are due once the pool is idle or MARKS_BATCH_MS has passed, so pages
  // landing close together share one e-ink refresh. Clears it when due.
  bool marksDue(uint32_t& pendingSinceMs) const;

 private:
  struct QueuedPage {
    std::string url;
    bool evict = false;
    bool revalidate = false;
    // Empty authorizationOrigin: the pool's credentials.
    std::string username;
    std::string password;
    std::string authorizationOrigin;
  };
  struct Worker {
#if defined(FREEINK_NET_WOLFSSL)
    // Declared before the prefetcher so it outlives the prefetch task.
    std::unique_ptr<freeink::SecureHttpClient> connection;
#endif
    OpdsPagePrefetcher prefetcher;
    unsigned long lastUseMs = 0;
    QueuedPage job;  // what the prefetcher is (or last was) running
  };

  bool queued(const std::string& url) const;
  bool running(const std::string& url) const;
  size_t runningCount() const;
  bool backingOff() const;
  void harvest(Worker& worker);
  bool startNext(Worker& worker);

  OpdsPageCache& cache;
  size_t pageMaxBytes;
  std::string username;
  std::string password;
  std::string authorizationOrigin;
  Worker workers[MAX_WORKERS];
  std::vector<QueuedPage> queue;
  std::string changedUrl;
  bool heapLimited = false;      // logs the heap skip once per stall
  bool active = false;           // something was started since the last idle log
  unsigned long failedAtMs = 0;  // millis() of the last network failure (0: none)
};
