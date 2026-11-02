#include <gtest/gtest.h>

#include "ReadingSessionStats.h"

// refreshGlobalStatsAfterOverlay() links these; the tests below never call it.
GlobalReadingStats GlobalReadingStats::load() { return {}; }
uint32_t GlobalReadingStats::localResetRevision() { return 0; }

namespace {
ReadingStatsDateTime at(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute, uint8_t second = 0) {
  return {{year, month, day}, hour, minute, second};
}
void interval(ReadingSessionDates& dates, ReadingStatsDateTime start, uint32_t seconds, uint8_t offset = 48) {
  dates.start(start, offset);
  auto end = start;
  addSecondsToReadingStatsDateTime(end, seconds);
  dates.accept(end, offset, seconds);
}
uint16_t streak(const GlobalReadingStats& global, ReadingStatsDate today) {
  return computeReadingHistoryCurrentStreak(global.readingHistoryAnchorDay, global.readingHistoryBits, &today);
}
}  // namespace

TEST(SessionDates, PausesAcrossMidnightCreditTheActualDaysAndTimeBuckets) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 20, 0), 600);
  // The same reader stays open while paused for several hours.
  interval(dates, at(2026, 10, 23, 8, 0), 600);
  commitReadingSession(book, global, 1200, &dates);
  EXPECT_EQ(global.totalReadingSeconds, 1200u);
  EXPECT_EQ(global.totalSessions, 1u);
  EXPECT_EQ(streak(global, {2026, 10, 23}), 2u);
  EXPECT_EQ(global.longestReadingStreak, 2u);
  EXPECT_EQ(book.dayOfWeekSeconds[3], 600u);  // Thursday
  EXPECT_EQ(book.dayOfWeekSeconds[4], 600u);  // Friday
  EXPECT_EQ(book.timeOfDaySeconds[0], 600u);  // Morning
  EXPECT_EQ(book.timeOfDaySeconds[2], 600u);  // Evening
  EXPECT_EQ(book.dayOfWeekSeconds, global.dayOfWeekSeconds);
  EXPECT_EQ(book.timeOfDaySeconds, global.timeOfDaySeconds);
  EXPECT_EQ(book.startDate.day, 22);
}

TEST(SessionDates, OnePageSpanningMidnightSplitsSecondsAtTheBoundary) {
  for (auto start : {at(2026, 12, 31, 23, 59, 55), at(2028, 2, 29, 23, 59, 55)}) {
    BookReadingStats book;
    GlobalReadingStats global;
    ReadingSessionDates dates;
    interval(dates, start, 10);
    auto end = start;
    addSecondsToReadingStatsDateTime(end, 10);
    commitReadingSession(book, global, 10, &dates);
    EXPECT_EQ(streak(global, end.date), 2u);
    EXPECT_EQ(global.dayOfWeekSeconds[readingStatsDayOfWeekIndex(start.date)], 5u);
    EXPECT_EQ(global.dayOfWeekSeconds[readingStatsDayOfWeekIndex(end.date)], 5u);
    EXPECT_EQ(global.timeOfDaySeconds[3], 10u);
  }
}

TEST(SessionDates, MultidayPausesDoNotManufactureReadingOnSkippedDays) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 20, 0), 60);
  interval(dates, at(2026, 10, 24, 8, 0), 60);
  commitReadingSession(book, global, 120, &dates);
  EXPECT_EQ(streak(global, {2026, 10, 24}), 1u);
  EXPECT_EQ(global.readingHistoryBits[0] & 7u, 5u);
  EXPECT_EQ(global.longestReadingStreak, 1u);
}

TEST(SessionDates, ClockCorrectionsAndUnavailableDatesKeepOnlyUndatedTotals) {
  BookReadingStats book;
  GlobalReadingStats global;
  ReadingSessionDates dates;
  dates.start(at(2026, 10, 22, 23, 0), 48);
  dates.accept(at(2026, 10, 23, 1, 0), 48, 60);  // Clock jumped forward.
  dates.start(at(2026, 10, 23, 1, 0), 48);
  dates.accept(at(2026, 10, 22, 23, 0), 48, 60);  // Clock jumped back.
  dates.start(at(2026, 10, 23, 1, 0), 48);
  dates.accept(at(2026, 10, 23, 1, 1), 49, 60);  // Offset changed.
  dates.start({}, 48);
  dates.accept(at(2026, 10, 23, 1, 1), 48, 60);
  dates.start(at(2026, 10, 23, 1, 0), 48);
  dates.accept({}, 48, 60);
  interval(dates, at(2026, 10, 23, 1, 0), 60, 255);  // Invalid offset.
  commitReadingSession(book, global, 360, &dates);
  EXPECT_EQ(global.totalReadingSeconds, 360u);
  EXPECT_EQ(global.readingHistoryAnchorDay, 0u);
  EXPECT_EQ(global.timeOfDaySeconds, (std::array<uint32_t, 4>{}));
  EXPECT_FALSE(book.startDate.isValid());
  // A working clock later in the same session can provide dated reading.
  interval(dates, at(2026, 10, 24, 8, 0), 120);
  BookReadingStats laterBook;
  GlobalReadingStats laterGlobal;
  commitReadingSession(laterBook, laterGlobal, 480, &dates);
  EXPECT_EQ(laterGlobal.totalReadingSeconds, 480u);
  EXPECT_EQ(streak(laterGlobal, {2026, 10, 24}), 1u);
  EXPECT_EQ(laterBook.startDate.day, 24);
  EXPECT_EQ(laterGlobal.timeOfDaySeconds[0], 120u);
}

TEST(SessionDates, CommitMergesExistingHistoryAndPreservesLongestStreak) {
  BookReadingStats book;
  GlobalReadingStats global;
  recordReadingSpanIntoHistory(global.readingHistoryAnchorDay, global.readingHistoryBits, at(2026, 10, 21, 12, 0), 60);
  global.longestReadingStreak = 35;
  book.startDate = {2026, 9, 1};
  book.startDateManual = true;
  ReadingSessionDates dates;
  interval(dates, at(2026, 10, 22, 20, 0), 60);
  interval(dates, at(2026, 10, 23, 8, 0), 60);
  commitReadingSession(book, global, 120, &dates);
  EXPECT_EQ(streak(global, {2026, 10, 23}), 3u);
  EXPECT_EQ(global.longestReadingStreak, 35u);
  EXPECT_EQ(book.startDate.month, 9);
}
