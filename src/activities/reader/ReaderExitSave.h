#pragma once
#include <string>

struct BookReadingStats;
struct GlobalReadingStats;

// The EPUB reader's exit writes (book stats, global stats, APP_STATE), held so
// they run while Home's first refresh is on the panel instead of before Home
// renders. ActivityManager flushes before any other screen enters, sleep
// flushes behind the sleep screen, and a shutdown handler flushes on every
// restart. While held, the stats loaders return these copies, so Home skips
// re-reading them from SD.
namespace ReaderExitSave {
void queue(const std::string& cachePath, const BookReadingStats* book, const GlobalReadingStats* global);
void flush();  // no-op when nothing is queued
const BookReadingStats* book(const std::string& cachePath);
const GlobalReadingStats* global();
}  // namespace ReaderExitSave
