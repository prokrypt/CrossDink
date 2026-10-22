#pragma once

#include <KOReaderSyncClient.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "StatsUploadClient.h"

struct BookReadingStats;

// Foreground helpers shared by reader sync and the explicit folder upload.
namespace ReadingSyncUpload {
// Only saved, mappable EPUB positions qualify. Never invent progress for an unread book.
bool prepareProgress(const std::string& path, KOReaderProgress& progress);
struct ExtrasResult {
  StatsUploadClient::Result stats = StatsUploadClient::Result::Skipped;
  StatsUploadClient::Result clippings = StatsUploadClient::Result::Skipped;
  bool success() const { return !StatsUploadClient::failed(stats) && !StatsUploadClient::failed(clippings); }
};
StatsUploadClient::Result globalStats();
// Reuse the explicit stats activity's existing buffer/client rather than allocating a second pair.
StatsUploadClient::Result globalStats(StatsUploadClient& client, char* payload, size_t capacity, const char* deviceId);
StatsUploadClient::Result stats(const std::string& path, const std::string& document);
// A book's saved stats when there are any to send (stats tracking on for it).
// Only a stats-file existence check and one small read; never loads the book.
bool loadBookStats(const std::string& path, BookReadingStats& stats);
// Targeted upload actions appear only with credentials, a server confirmed to
// accept stats and clippings (an unknown custom server waits for Authenticate
// or a first upload), and the matching What to Sync toggle on.
bool canSyncReadingStats();
bool canSyncClippings();
ExtrasResult extras(const std::string& path, const std::string& document);
}  // namespace ReadingSyncUpload
