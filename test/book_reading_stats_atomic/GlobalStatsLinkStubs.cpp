#include <ReadingStatsUtils.h>

#include "ReaderExitSave.h"

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>&,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>&, const ReadingStatsDateTime&,
                                  uint32_t) {}
void recordReadingSpanIntoHistory(uint32_t&, std::array<uint8_t, READING_HISTORY_BYTES>&, const ReadingStatsDateTime&,
                                  uint32_t) {}
void mergeReadingHistory(uint32_t&, std::array<uint8_t, READING_HISTORY_BYTES>&, uint32_t,
                         const std::array<uint8_t, READING_HISTORY_BYTES>&) {}
uint16_t computeReadingHistoryLongestStreak(uint32_t, const std::array<uint8_t, READING_HISTORY_BYTES>&) { return 0; }
uint16_t computeReadingHistoryCurrentStreak(uint32_t, const std::array<uint8_t, READING_HISTORY_BYTES>&,
                                            const ReadingStatsDate*) {
  return 0;
}

namespace ReaderExitSave {
const GlobalReadingStats* global() { return nullptr; }
}  // namespace ReaderExitSave
