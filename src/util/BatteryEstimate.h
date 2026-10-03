#pragma once

#include <FrontlightGamma.h>

#include <algorithm>
#include <cstdint>

namespace BatteryEstimate {

// The X4 Pro LED duty (0-1023) for a brightness %: FrontlightManager's own
// perceptual curve on the 10-bit range. The LED draw follows this duty, not
// the %: 1% is 1/1023 of full, 10% is 23, 50% is 325. Warmth only splits this
// duty between the warm and cool LEDs, so it leaves the total alone.
inline uint16_t lightDuty(const uint8_t pct) { return FrontlightGamma::perceptualDuty(pct, 1023); }

// The light's drain per duty unit from one Wi-Fi state's light-off and light-on
// rates (lightScaledRate's arguments), < 0 when that pair can't tell: a side
// missing, or the light-on stretches drained no more.
inline float ledSlope(const float offRate, const float onRate, const float avgDuty) {
  return offRate > 0 && onRate > offRate ? (onRate - offRate) / std::max(avgDuty, 1.0f) : -1.0f;
}

// Awake drain at `light` % brightness from the logged drain with the light off
// and on (same unit, 0 = not enough data) and the light-on stretches' average
// duty (lightDuty). The light's share scales with the duty. The light only adds
// drain: when the light-on stretches drained less (the light-off ones held
// heavier work), it adds nothing rather than making a brighter light last longer.
// The LED draws the same with Wi-Fi on or off, so its share per duty unit is the
// smaller of this pair's and the other Wi-Fi state's ledSlope (otherSlope, < 0 =
// unknown): a bigger one means that state's light-on stretches held heavier work
// (Wi-Fi remote idle with the light off, reading with it on), and it is at most
// maxSlope, the LED's own full-duty drain. The light-off drain is at least the
// light-on drain less the light's share. With no share, a missing side takes the
// other one, so the result is always awake drain (0 = neither side known).
inline float lightScaledRate(const float offRate, const float onRate, const float avgDuty, const uint8_t light,
                             const float otherSlope = -1.0f, const float maxSlope = 1e9f) {
  float slope = ledSlope(offRate, onRate, avgDuty);
  if (otherSlope >= 0 && (slope < 0 || otherSlope < slope)) slope = otherSlope;
  slope = std::min(slope, maxSlope);
  if (slope >= 0) {
    const float base = std::max(offRate, onRate - slope * avgDuty);
    if (base > 0) return base + slope * lightDuty(light);
  }
  // Light-off drain is a lower bound with the light on.
  return offRate > 0 ? offRate : onRate;
}

}  // namespace BatteryEstimate
