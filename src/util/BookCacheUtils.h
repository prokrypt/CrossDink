#pragma once

#include <cstdint>
#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Clears derived reading cache files while preserving user-owned state such as
// progress and per-book stats. Returns false if the cache clear or state
// preservation fails.
// A replaced EPUB (new content key) also gets its progress and stats carried
// over from the key library.idx recorded; the Library builder, which holds that
// index open, passes false and carries them itself.
bool clearBookCachePreservingUserState(const std::string& path, bool carryFromIndexedKey = true);

// Copies progress and per-book stats from the cache of an EPUB's previous
// content (oldKey; 0 looks it up in library.idx) into its current cache, unless
// that already has progress.
void carryEpubReadingState(const std::string& path, uint64_t oldKey);

// Clears a known book cache directory while preserving dictionary lookup
// history and per-book stats.
bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
