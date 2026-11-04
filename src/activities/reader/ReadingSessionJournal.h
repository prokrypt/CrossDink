#pragma once
#include <cstdint>

#include "ReadingStatsUtils.h"

// An outbox alongside existing stats. One record per continuous session, keyed
// by a random ID that survives upload retries; only explicit acknowledgment
// permits removal. Nothing is written while paging: a session reaches the SD at
// the reader's existing stats flush points (exit, tracking toggle, restart) or
// when a pause ends it and the next accepted interval starts a new one.
namespace ReadingSessionJournal {
struct Record {
  char id[33]{};
  char document[33]{};
  uint64_t startedAt = 0;
  uint64_t endedAt = 0;
  uint32_t seconds = 0;
  int16_t utcOffsetMinutes = 0;
};
using Upload = bool (*)(void*, const Record&);
bool uploadPending(Upload upload, void* context);
bool save(const Record& record);

class Session {
  Record pending{};
  ReadingStatsDateTime intervalStart{};
  uint8_t intervalOffset = 255;

 public:
  void start(const ReadingStatsDateTime& local, uint8_t offset) {
    intervalStart = local;
    intervalOffset = offset;
  }
  // Uses the same accepted page intervals as the existing daily counters.
  bool accept(const char* bookPath, const ReadingStatsDateTime& end, uint8_t offset, uint32_t seconds);
  // Saves the live session (if it reached ten seconds) and starts a new identity.
  bool flush();
};
}  // namespace ReadingSessionJournal
