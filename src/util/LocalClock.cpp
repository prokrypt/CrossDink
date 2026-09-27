#include "LocalClock.h"

#include <HalClock.h>

#include "CrossPointSettings.h"
#include "DaylightSaving.h"

namespace LocalClock {

uint8_t offsetQAtUtc(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                     const uint8_t minute) {
  return DaylightSaving::offsetQAt(SETTINGS.clockUtcOffsetQ, SETTINGS.clockDstRule, year, month, day, hour, minute);
}

uint8_t currentOffsetQ(const uint8_t standardOffsetQ) {
  uint16_t year = 0;
  uint8_t month = 0, day = 0, hour = 0, minute = 0;
  if (!halClock.getDateTime(year, month, day, hour, minute)) return DaylightSaving::clampOffsetQ(standardOffsetQ);
  return DaylightSaving::offsetQAt(standardOffsetQ, SETTINGS.clockDstRule, year, month, day, hour, minute);
}

uint8_t currentOffsetQ() { return currentOffsetQ(SETTINGS.clockUtcOffsetQ); }

}  // namespace LocalClock
