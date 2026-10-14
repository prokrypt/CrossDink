#pragma once
#include <string>

struct BookReadingStats;
struct GlobalReadingStats;

// The readers' exit writes, held until a flush point: the EPUB and XTC
// readers' book and global stats and APP_STATE here, every reader's progress
// and the EPUB percent in HeldWrites. Sleep flushes behind the sleep screen, a
// shutdown handler on every restart, and ActivityManager before any screen but
// Home, Library and the readers enters (those read the held copies); so do low
// battery, the remote FLUSH and the book actions. While held, the stats loaders
// return these copies.
namespace ReaderExitSave {
void queue(const std::string& cachePath, const BookReadingStats* book, const GlobalReadingStats* global);
// The held stats and APP_STATE only: a reader opening a book writes them first,
// so its own saves during the session cannot be overwritten by an older copy.
bool flushStats();  // no-op when nothing is queued; true when it wrote
bool flush();       // flushStats() and the held HeldWrites files
const BookReadingStats* book(const std::string& cachePath);
const GlobalReadingStats* global();
}  // namespace ReaderExitSave
