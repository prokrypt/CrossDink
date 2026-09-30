#pragma once

#include <atomic>
#include <cstddef>
#include <string>

#include "network/HttpDownloader.h"
#include "util/WorkerTask.h"

/**
 * Downloads one OPDS book to SD on a background task (the UI core), so the
 * main loop keeps reading input (Back, Home, the Cancel button) and drawing
 * progress while the transfer runs.
 *
 * Same ownership rules as OpdsPagePrefetcher: the activity owns the object,
 * starts one job at a time, and must not start another network request while
 * it runs. join() and the destructor block until the task has exited. A
 * cancelled download removes its .part file (HttpDownloader stages to .part),
 * so an existing copy of the book is left untouched. A failed one keeps it for
 * a resume; the owner removes it when the user gives up.
 */
class OpdsBookDownloader {
 public:
  struct Request {
    std::string url;
    std::string path;
    std::string username;
    std::string password;
    std::string authorizationOrigin;
    // Continue the .part file left by a failed attempt (HttpDownloader falls
    // back to byte 0 when the server ignores the Range request).
    bool resume = false;
    // In: the failed attempt's validator, sent as If-Range. Out: this response's.
    std::string validator;
  };

  OpdsBookDownloader() = default;
  ~OpdsBookDownloader();
  OpdsBookDownloader(const OpdsBookDownloader&) = delete;
  OpdsBookDownloader& operator=(const OpdsBookDownloader&) = delete;

  // False when a job is still running or the task could not start.
  bool start(Request&& request);

  // True while the background task is alive.
  bool running() const { return task.running(); }
  void cancel() { cancelRequested.store(true, std::memory_order_release); }
  bool cancelling() const { return cancelRequested.load(std::memory_order_acquire); }
  // Blocks the caller until the background task has exited.
  void join() { task.join(); }

  // Progress, readable from any task while the job runs.
  size_t downloaded() const { return bytesDone.load(std::memory_order_acquire); }
  size_t total() const { return bytesTotal.load(std::memory_order_acquire); }
  // True once the first body byte has arrived (before that: DNS, TLS,
  // redirects and the server preparing the file).
  bool receiving() const { return firstByteSeen.load(std::memory_order_acquire); }

  // Valid after the task has exited.
  HttpDownloader::DownloadError result() const { return outcome; }
  const std::string& path() const { return job.path; }
  const std::string& validator() const { return job.validator; }

 private:
  void run();

  Request job;
  HttpDownloader::DownloadError outcome = HttpDownloader::HTTP_ERROR;  // written before running() clears
  std::atomic<size_t> bytesDone{0};
  std::atomic<size_t> bytesTotal{0};
  std::atomic<bool> firstByteSeen{false};
  std::atomic<bool> cancelRequested{false};
  WorkerTask task;
};
