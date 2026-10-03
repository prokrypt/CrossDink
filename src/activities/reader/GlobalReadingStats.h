#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Cumulative reading statistics across all books, persisted to
// /.crossdink/global_stats_dink.bin (two slots; global_stats.bin is read until the first save).
struct GlobalReadingStats {
  uint32_t totalSessions = 0;        // Total book-open events across all books
  uint32_t totalReadingSeconds = 0;  // Accumulated reading time across all books
  uint32_t totalPagesTurned = 0;     // Total forward page turns after the dwell threshold
  uint32_t completedBooks = 0;       // Books manually marked as finished
  std::array<uint32_t, READING_TIME_BUCKET_COUNT> timeOfDaySeconds{};
  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT> dayOfWeekSeconds{};
  uint32_t readingHistoryAnchorDay = 0;
  std::array<uint8_t, READING_HISTORY_BYTES> readingHistoryBits{};
  uint16_t longestReadingStreak = 0;

  static constexpr uint8_t CURRENT_FILE_VERSION = 3;
  static constexpr size_t CURRENT_FILE_SIZE = 159;
  static constexpr size_t MIN_SUPPORTED_FILE_SIZE = 13;

  // Loads the newest valid stats (see above). Returns default-constructed
  // stats if the file is missing or the version byte does not match.
  static GlobalReadingStats load();

  // Returns true when the optional synced stats directory exists.
  static bool hasSyncedStats();

  // Loads this device's local stats plus one synced stats file per other device
  // from /.crossdink/synced_stats/. A stale file matching this device's MAC is
  // skipped to avoid double counting.
  static GlobalReadingStats loadAggregated();

  // Adds synced device stats to an already-loaded local stats snapshot. Use this
  // when the local stats may include in-memory changes that are not saved yet.
  static GlobalReadingStats loadAggregated(const GlobalReadingStats& localStats);

  // Saves stats in place to the older of the two slots global_stats_dink.bin
  // and global_stats_dink.bin.bak (TwoSlotFile.h). global_stats.bin is left as
  // is, for older firmware.
  void save() const;

  // Copies this device's stats payload (newest valid slot, without the slot
  // trailer) into out. Returns its size, or 0 when there is none.
  static size_t readLocalFile(std::array<uint8_t, CURRENT_FILE_SIZE>& out);

  // Replaces the stats files (both slots and global_stats.bin) with fresh empty stats
  // without touching the backups in /.crossink-stats-backup.
  static bool resetLocal();

  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);
  uint16_t currentReadingStreak(const ReadingStatsDate* today) const;
  uint16_t displayLongestReadingStreak() const;
};
