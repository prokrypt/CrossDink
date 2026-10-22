#include "ReaderExitSave.h"

#include <Arduino.h>
#include <Logging.h>

#include <atomic>

#include "BookReadingStats.h"
#include "CrossPointState.h"
#include "DailyReadingStats.h"
#include "GlobalReadingStats.h"
#include "HeldWrites.h"
#include "sync/SyncPending.h"

namespace {
// Static, not heap: one reader exit is held at a time (~300 B). Written only in
// queue() (reader onExit, render lock held) and left untouched until flush()
// clears `pending`, so render-task loads can copy them without a lock.
std::atomic<bool> pending{false};
std::string heldCachePath;
BookReadingStats heldBook;
GlobalReadingStats heldGlobal;
bool hasBook = false;
bool hasGlobal = false;
std::string heldSyncPath;
uint8_t heldSyncBits = 0;
}  // namespace

void ReaderExitSave::queue(const std::string& cachePath, const BookReadingStats* book, const GlobalReadingStats* global,
                           const std::string& bookPath, const uint8_t syncBits) {
  flushStats();  // never drop an older held write
  heldCachePath = cachePath;
  heldSyncPath = bookPath;
  heldSyncBits = syncBits;
  hasBook = book != nullptr;
  if (book) heldBook = *book;
  hasGlobal = book && global;
  if (hasGlobal) heldGlobal = *global;
  pending.store(true, std::memory_order_release);
}

bool ReaderExitSave::flushStats() {
  if (!pending.load(std::memory_order_acquire)) return false;
  // cppcheck-suppress unreadVariable ; read only by LOG_DBG, compiled out in release
  const unsigned long start = millis();
  // Same order as the reader always used: global stats only once the book's landed.
  if (hasBook && heldBook.save(heldCachePath) && hasGlobal) heldGlobal.save();
  if (heldSyncBits) SyncPending::mark(heldSyncPath, heldSyncBits);
  APP_STATE.saveToFile();
  pending.store(false, std::memory_order_release);
  LOG_DBG("RXS", "Reader exit writes flushed in %lu ms", millis() - start);
  return true;
}

bool ReaderExitSave::flush() {
  const bool wrote = flushStats();
  if (!DailyReadingStats::flush()) LOG_ERR("RXS", "Daily reading totals stay in RAM until the next flush");
  HeldWrites::flushFiles();
  return wrote;
}

const BookReadingStats* ReaderExitSave::book(const std::string& cachePath) {
  return pending.load(std::memory_order_acquire) && hasBook && cachePath == heldCachePath ? &heldBook : nullptr;
}

const GlobalReadingStats* ReaderExitSave::global() {
  return pending.load(std::memory_order_acquire) && hasGlobal ? &heldGlobal : nullptr;
}
