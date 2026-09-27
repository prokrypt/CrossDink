#pragma once

#include <cstdint>

// Daylight-saving rules and small calendar helpers for the clock's biased
// quarter-hour UTC offset (48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00). The
// RTC runs in UTC; a rule decides whether the user's standard offset gets one
// extra hour at a given UTC instant. Kept free of hardware and settings so
// host tests can pin the transition boundaries.
namespace DaylightSaving {

enum Rule : uint8_t {
  RULE_OFF = 0,
  RULE_US_CANADA = 1,    // 2nd Sunday in March 02:00 -> 1st Sunday in November 02:00, local time
  RULE_EUROPE = 2,       // last Sunday in March 01:00 UTC -> last Sunday in October 01:00 UTC
  RULE_AUSTRALIA = 3,    // 1st Sunday in October 02:00 -> 1st Sunday in April 03:00, local time
  RULE_NEW_ZEALAND = 4,  // last Sunday in September 02:00 -> 1st Sunday in April 03:00, local time
  RULE_COUNT
};

constexpr uint8_t kMaxOffsetQ = 104;
constexpr uint8_t kOffsetBias = 48;
constexpr uint8_t kDstOffsetQ = 4;  // one hour

constexpr bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

constexpr uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  constexpr uint8_t kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month < 1 || month > 12 ? 0 : (month == 2 && isLeapYear(year) ? 29 : kDays[month - 1]);
}

constexpr bool isValidDate(const uint16_t year, const uint8_t month, const uint8_t day) {
  return year >= 1970 && day >= 1 && day <= daysInMonth(year, month);
}

// 0 = Sunday (Sakamoto's method).
constexpr uint8_t weekday(uint16_t year, const uint8_t month, const uint8_t day) {
  constexpr uint8_t kMonthKey[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (month < 3) year--;
  return static_cast<uint8_t>((year + year / 4 - year / 100 + year / 400 + kMonthKey[month - 1] + day) % 7);
}

// Day of month of the nth Sunday (n >= 1), or of the last Sunday when n == 0.
constexpr uint8_t sundayOfMonth(const uint16_t year, const uint8_t month, const uint8_t n) {
  if (n == 0) {
    const uint8_t last = daysInMonth(year, month);
    return static_cast<uint8_t>(last - weekday(year, month, last));
  }
  const uint8_t firstSunday = static_cast<uint8_t>(1 + (7 - weekday(year, month, 1)) % 7);
  return static_cast<uint8_t>(firstSunday + 7 * (n - 1));
}

constexpr int32_t minuteOfYear(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                               const uint8_t minute) {
  int32_t days = day - 1;
  for (uint8_t m = 1; m < month; m++) days += daysInMonth(year, m);
  return days * 1440 + hour * 60 + minute;
}

constexpr uint8_t clampOffsetQ(const uint8_t offsetQ) { return offsetQ > kMaxOffsetQ ? kMaxOffsetQ : offsetQ; }

constexpr int offsetMinutes(const uint8_t offsetQ) {
  return (static_cast<int>(clampOffsetQ(offsetQ)) - kOffsetBias) * 15;
}

// True when `rule` puts a zone with standard offset `standardOffsetQ` on
// daylight time at the given UTC instant. Transitions stated in local standard
// time are converted to UTC with the standard offset; none of the supported
// rules switch near New Year, so comparing minutes within one year is enough.
constexpr bool isActive(const uint8_t rule, const uint8_t standardOffsetQ, const uint16_t year, const uint8_t month,
                        const uint8_t day, const uint8_t hour, const uint8_t minute) {
  if (rule == RULE_OFF || rule >= RULE_COUNT || !isValidDate(year, month, day)) return false;
  const int32_t now = minuteOfYear(year, month, day, hour, minute);
  const int standard = offsetMinutes(standardOffsetQ);
  const auto utcOf = [year, standard](const uint8_t m, const uint8_t d, const uint8_t localStandardHour) {
    return minuteOfYear(year, m, d, localStandardHour, 0) - standard;
  };
  switch (rule) {
    case RULE_US_CANADA:
      // Ends at 02:00 daylight time, which is 01:00 standard time.
      return now >= utcOf(3, sundayOfMonth(year, 3, 2), 2) && now < utcOf(11, sundayOfMonth(year, 11, 1), 1);
    case RULE_EUROPE:
      return now >= minuteOfYear(year, 3, sundayOfMonth(year, 3, 0), 1, 0) &&
             now < minuteOfYear(year, 10, sundayOfMonth(year, 10, 0), 1, 0);
    case RULE_AUSTRALIA:
      // Southern hemisphere: daylight time spans New Year. Ends at 03:00 daylight = 02:00 standard.
      return now >= utcOf(10, sundayOfMonth(year, 10, 1), 2) || now < utcOf(4, sundayOfMonth(year, 4, 1), 2);
    case RULE_NEW_ZEALAND:
      return now >= utcOf(9, sundayOfMonth(year, 9, 0), 2) || now < utcOf(4, sundayOfMonth(year, 4, 1), 2);
    default:
      return false;
  }
}

// Biased offset in effect at the given UTC instant.
constexpr uint8_t offsetQAt(const uint8_t standardOffsetQ, const uint8_t rule, const uint16_t year, const uint8_t month,
                            const uint8_t day, const uint8_t hour, const uint8_t minute) {
  const uint8_t standard = clampOffsetQ(standardOffsetQ);
  if (!isActive(rule, standard, year, month, day, hour, minute)) return standard;
  return clampOffsetQ(static_cast<uint8_t>(standard + kDstOffsetQ));
}

// Moves a date/time by a signed number of minutes (|delta| up to a few days).
constexpr void addMinutes(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute,
                          const int delta) {
  int total = hour * 60 + minute + delta;
  while (total < 0) {
    total += 1440;
    if (day > 1) {
      day--;
    } else {
      if (month > 1) {
        month--;
      } else {
        month = 12;
        year--;
      }
      day = daysInMonth(year, month);
    }
  }
  while (total >= 1440) {
    total -= 1440;
    if (day < daysInMonth(year, month)) {
      day++;
    } else {
      day = 1;
      if (month < 12) {
        month++;
      } else {
        month = 1;
        year++;
      }
    }
  }
  hour = static_cast<uint8_t>(total / 60);
  minute = static_cast<uint8_t>(total % 60);
}

// Converts a local wall-clock time back to UTC. In the repeated hour when
// daylight time ends, the standard-time reading wins.
constexpr void localToUtc(const uint8_t standardOffsetQ, const uint8_t rule, uint16_t& year, uint8_t& month,
                          uint8_t& day, uint8_t& hour, uint8_t& minute) {
  addMinutes(year, month, day, hour, minute, -offsetMinutes(standardOffsetQ));
  if (isActive(rule, standardOffsetQ, year, month, day, hour, minute)) {
    uint16_t y = year;
    uint8_t mo = month, d = day, h = hour, mi = minute;
    addMinutes(y, mo, d, h, mi, -60);
    if (isActive(rule, standardOffsetQ, y, mo, d, h, mi)) {
      year = y;
      month = mo;
      day = d;
      hour = h;
      minute = mi;
    }
  }
}

}  // namespace DaylightSaving
