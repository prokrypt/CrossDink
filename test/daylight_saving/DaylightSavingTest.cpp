#include <gtest/gtest.h>

#include "DaylightSaving.h"

using namespace DaylightSaving;

namespace {
constexpr uint8_t offsetQ(const int hours) { return static_cast<uint8_t>(kOffsetBias + hours * 4); }

bool active(const uint8_t rule, const int standardHours, const uint8_t month, const uint8_t day, const uint8_t hour,
            const uint8_t minute = 0) {
  return isActive(rule, offsetQ(standardHours), 2026, month, day, hour, minute);
}
}  // namespace

TEST(DaylightSaving, SundayOfMonth) {
  EXPECT_EQ(sundayOfMonth(2026, 3, 2), 8);
  EXPECT_EQ(sundayOfMonth(2026, 3, 0), 29);
  EXPECT_EQ(sundayOfMonth(2026, 11, 1), 1);
  EXPECT_EQ(sundayOfMonth(2026, 9, 0), 27);
  EXPECT_EQ(sundayOfMonth(2024, 2, 0), 25);
}

TEST(DaylightSaving, OffNeverShifts) {
  EXPECT_FALSE(active(RULE_OFF, -5, 7, 1, 12));
  EXPECT_EQ(offsetQAt(offsetQ(-5), RULE_OFF, 2026, 7, 1, 12, 0), offsetQ(-5));
}

TEST(DaylightSaving, UsCanadaSwitchesAtTwoAmLocal) {
  // New York: starts 2026-03-08 02:00 EST (07:00 UTC), ends 2026-11-01 02:00 EDT (06:00 UTC).
  EXPECT_FALSE(active(RULE_US_CANADA, -5, 3, 8, 6, 59));
  EXPECT_TRUE(active(RULE_US_CANADA, -5, 3, 8, 7, 0));
  EXPECT_TRUE(active(RULE_US_CANADA, -5, 11, 1, 5, 59));
  EXPECT_FALSE(active(RULE_US_CANADA, -5, 11, 1, 6, 0));
  // Los Angeles switches three hours later in UTC.
  EXPECT_FALSE(active(RULE_US_CANADA, -8, 3, 8, 9, 59));
  EXPECT_TRUE(active(RULE_US_CANADA, -8, 3, 8, 10, 0));
  EXPECT_EQ(offsetQAt(offsetQ(-5), RULE_US_CANADA, 2026, 7, 1, 12, 0), offsetQ(-4));
}

TEST(DaylightSaving, EuropeSwitchesAtOneAmUtcEverywhere) {
  for (const int standardHours : {0, 1, 2}) {
    EXPECT_FALSE(active(RULE_EUROPE, standardHours, 3, 29, 0, 59));
    EXPECT_TRUE(active(RULE_EUROPE, standardHours, 3, 29, 1, 0));
    EXPECT_TRUE(active(RULE_EUROPE, standardHours, 10, 25, 0, 59));
    EXPECT_FALSE(active(RULE_EUROPE, standardHours, 10, 25, 1, 0));
  }
}

TEST(DaylightSaving, SouthernRulesSpanNewYear) {
  // Sydney: ends 2026-04-05 03:00 AEDT (04-04 16:00 UTC), starts 2026-10-04 02:00 AEST (10-03 16:00 UTC).
  EXPECT_TRUE(active(RULE_AUSTRALIA, 10, 1, 15, 0));
  EXPECT_TRUE(active(RULE_AUSTRALIA, 10, 4, 4, 15, 59));
  EXPECT_FALSE(active(RULE_AUSTRALIA, 10, 4, 4, 16, 0));
  EXPECT_FALSE(active(RULE_AUSTRALIA, 10, 10, 3, 15, 59));
  EXPECT_TRUE(active(RULE_AUSTRALIA, 10, 10, 3, 16, 0));
  // Auckland: ends 2026-04-05 03:00 NZDT (04-04 14:00 UTC), starts 2026-09-27 02:00 NZST (09-26 14:00 UTC).
  EXPECT_TRUE(active(RULE_NEW_ZEALAND, 12, 4, 4, 13, 59));
  EXPECT_FALSE(active(RULE_NEW_ZEALAND, 12, 4, 4, 14, 0));
  EXPECT_FALSE(active(RULE_NEW_ZEALAND, 12, 9, 26, 13, 59));
  EXPECT_TRUE(active(RULE_NEW_ZEALAND, 12, 9, 26, 14, 0));
}

TEST(DaylightSaving, LocalToUtcUndoesTheOffset) {
  uint16_t year = 2026;
  uint8_t month = 7, day = 1, hour = 12, minute = 30;
  localToUtc(offsetQ(1), RULE_EUROPE, year, month, day, hour, minute);
  EXPECT_EQ(hour, 10);
  EXPECT_EQ(minute, 30);

  year = 2026, month = 1, day = 1, hour = 0, minute = 15;
  localToUtc(offsetQ(1), RULE_EUROPE, year, month, day, hour, minute);
  EXPECT_EQ(year, 2025);
  EXPECT_EQ(month, 12);
  EXPECT_EQ(day, 31);
  EXPECT_EQ(hour, 23);

  // Repeated hour at the end of daylight time resolves to standard time.
  year = 2026, month = 10, day = 25, hour = 2, minute = 30;
  localToUtc(offsetQ(1), RULE_EUROPE, year, month, day, hour, minute);
  EXPECT_EQ(hour, 1);
  EXPECT_EQ(minute, 30);
}

TEST(DaylightSaving, LocalToUtcPacificWinterAndSummer) {
  // WebDAV mtime check: FAT local 2026-01-01 00:00 (PST) is 08:00 GMT; 2026-07-01 00:00 (PDT) is 07:00 GMT.
  uint16_t year = 2026;
  uint8_t month = 1, day = 1, hour = 0, minute = 0;
  localToUtc(offsetQ(-8), RULE_US_CANADA, year, month, day, hour, minute);
  EXPECT_EQ(day, 1);
  EXPECT_EQ(hour, 8);

  month = 7, day = 1, hour = 0, minute = 0;
  localToUtc(offsetQ(-8), RULE_US_CANADA, year, month, day, hour, minute);
  EXPECT_EQ(hour, 7);
}

TEST(DaylightSaving, AddMinutesCarriesAcrossMonthsAndLeapDays) {
  uint16_t year = 2024;
  uint8_t month = 2, day = 28, hour = 23, minute = 30;
  addMinutes(year, month, day, hour, minute, 60);
  EXPECT_EQ(month, 2);
  EXPECT_EQ(day, 29);
  EXPECT_EQ(hour, 0);
  EXPECT_EQ(minute, 30);
  addMinutes(year, month, day, hour, minute, 24 * 60);
  EXPECT_EQ(month, 3);
  EXPECT_EQ(day, 1);
}
