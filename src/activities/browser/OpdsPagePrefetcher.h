#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "OpdsPageCache.h"
#include "util/WorkerTask.h"

namespace freeink {
class SecureHttpClient;
}

/**
 * Downloads one OPDS feed page on a background task (core 0) so the next page
 * is ready in PSRAM before the user asks for it.
 *
 * Single job at a time. The main loop owns the object and must not start a
 * second network request while a job runs: cancel()/join() first. join() and
 * the destructor block until the task has exited, so the object can then be
 * destroyed safely. HttpDownloader and the TLS stack are never used from two
 * tasks at once because every foreground fetch joins this job first.
 */
class OpdsPagePrefetcher {
 public:
  struct Request {
    std::string url;
    std::string username;
    std::string password;
    std::string authorizationOrigin;
    // The activity's kept-alive feed connection (may be null). The task owns
    // it until join(); the main loop never touches it while a job runs.
    freeink::SecureHttpClient* connection = nullptr;
  };

  OpdsPagePrefetcher() = default;
  ~OpdsPagePrefetcher();
  OpdsPagePrefetcher(const OpdsPagePrefetcher&) = delete;
  OpdsPagePrefetcher& operator=(const OpdsPagePrefetcher&) = delete;

  // Starts downloading request.url into a PSRAM buffer capped at maxBytes.
  // Returns false when a job is still running or the task could not start.
  bool start(Request&& request, size_t maxBytes);

  // True while the background task is alive.
  bool running() const { return task.running(); }
  // URL of the running or last finished job; stable while running.
  const std::string& url() const { return job.url; }

  void cancel() { cancelRequested.store(true, std::memory_order_release); }
  // Blocks the caller until the background task has exited.
  void join() const { task.join(); }

  // After join(): moves a successfully downloaded page into cache.
  void harvestInto(OpdsPageCache& cache);

 private:
  void run();

  Request job;
  OpdsPageBuffer page;
  bool succeeded = false;  // written by the task before running() clears
  std::atomic<bool> cancelRequested{false};
  WorkerTask task;
};
