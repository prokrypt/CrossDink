#pragma once

#include <cstdint>
#include <string>

class Epub;
struct RecentBook;

// Helpers for loading and displaying recent-book reading progress.
// Progress values are percentages in the 0-100 range; negative means unknown.
//
// Example:
//   const float progress = RecentBookProgress::loadPercent(book);
//   const std::string label = RecentBookProgress::formatPercent(progress);
namespace RecentBookProgress {
// KOSync state of an EPUB, shown next to its % (Home) and left of its Library row icon.
enum class SyncBadge : uint8_t { None, Synced, Unsynced, ServerAhead };
// The book's own % (basis points) when a KOSync check last found the server
// holding it; serverAhead: the server was further on. kNever: no check yet.
struct SyncMark {
  static constexpr uint16_t kNever = 0xFFFF;
  uint16_t bp = kNever;
  bool serverAhead = false;
};

// Loads the saved reading percentage for a recent EPUB, XTC, TXT, or Markdown book.
// Returns -1.0f when progress is unavailable or the cache cannot be read.
float loadPercent(const RecentBook& book);
// Loads the cached EPUB reading percentage without opening the EPUB.
// Returns -1.0f when progress is unavailable or the cache cannot be read.
// mark: the KOSync mark saved with it (kNever when none).
float loadCachedEpubPercent(const RecentBook& book, SyncMark* mark = nullptr);
// Same, from a known EPUB cache folder; no read of the book itself.
float loadCachedEpubPercentAt(const std::string& cachePath, SyncMark* mark = nullptr);
// Only the saved % (-1: none) and KOSync mark of an EPUB cache folder: one small read.
float loadSavedEpubPercent(const std::string& cachePath, SyncMark& mark);
// Saves a cached EPUB reading percentage for fast Home screen rendering.
void saveCachedEpubPercent(const std::string& cachePath, float progress);
// Calculates and saves cached EPUB progress from a reader position.
// hold: a reader exit; the write waits for the next flush point (HeldWrites).
void saveCachedEpubPercent(const Epub& epub, int spineIndex, int currentPage, int pageCount, bool hold = false);
// The cached % of a reader position, in basis points.
uint16_t basisPoints(const Epub& epub, int spineIndex, int currentPage, int pageCount);
// Any task: a KOSync check found the server holding this book at bp (its own %)
// or further on. Kept in RAM until flushSyncMarks(); a newer note replaces it.
void noteSynced(const std::string& cachePath, uint16_t bp, bool serverAhead);
// Sleep, restart, OTA, USB Drive, File Transfer: saves held marks with the books' %.
void flushSyncMarks();
// The badge for a book at progress (0-100) with its saved mark, or a newer held
// one. None without KOSync credentials or for a book never checked.
SyncBadge syncBadge(const std::string& cachePath, float progress, SyncMark mark);
// Returns true when progress contains a known 0-100 percentage.
bool hasPercent(float progress);
// Formats progress as a rounded percentage string such as "42%".
// Returns an empty string when progress is unknown.
std::string formatPercent(float progress);
}  // namespace RecentBookProgress
