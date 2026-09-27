#pragma once

#include <cstdint>

// Local wall-clock offset for the RTC's UTC time: the user's standard UTC
// offset plus an hour while their daylight-saving rule is in effect. Values are
// biased quarter-hours, as HalClock::formatTime/formatDate expect.
namespace LocalClock {

// Offset in effect at the given UTC instant.
uint8_t offsetQAtUtc(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t minute);

// Offset in effect now for `standardOffsetQ` (the setting being previewed or
// the saved one). Falls back to the standard offset when the RTC has no date.
uint8_t currentOffsetQ(uint8_t standardOffsetQ);
uint8_t currentOffsetQ();

}  // namespace LocalClock
