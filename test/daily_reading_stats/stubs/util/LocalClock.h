#pragma once
#include <CrossPointSettings.h>

#include <cstdint>
// The test settings' fixed offset; daylight saving is not under test here.
namespace LocalClock {
inline uint8_t offsetQAtUtc(uint16_t, uint8_t, uint8_t, uint8_t, uint8_t) { return SETTINGS.clockUtcOffsetQ; }
}  // namespace LocalClock
