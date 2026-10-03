#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace BatteryEstimate {

// The X4 Pro LED duty (0-1023) for a brightness %: FrontlightManager's
// perceptual curve (GAMMA_TABLE, round(65535 * (pct/100)^1.6554), scaled to
// 10 bits, at least 1 LSB when lit). The LED draw follows this duty, not the
// %: 1% is 1/1023 of full, 10% is 23, 50% is 325. Warmth only splits this duty between the warm and cool LEDs, so it
// leaves the total alone. Keep in step with FrontlightManager.cpp's GAMMA_TABLE.
inline uint16_t lightDuty(const uint8_t pct) {
  if (pct == 0) return 0;
  const uint32_t g = std::lround(65535.0f * std::pow(std::min<uint8_t>(pct, 100) / 100.0f, 1.6554f));
  return static_cast<uint16_t>(std::max<uint32_t>((1023u * g + 32767u) / 65535u, 1u));
}

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
// The LED draws the same with Wi-Fi on or off, so when this pair can't tell the
// light's share, the other Wi-Fi state's ledSlope (< 0 = unknown) gives it, and
// a missing light-off rate is the light-on one less that share.
inline float lightScaledRate(const float offRate, const float onRate, const float avgDuty, const uint8_t light,
                             const float otherSlope = -1.0f) {
  if (otherSlope >= 0 && ledSlope(offRate, onRate, avgDuty) < 0) {
    const float off = offRate > 0 ? offRate : onRate - otherSlope * avgDuty;
    if (off > 0) return off + otherSlope * lightDuty(light);
  }
  if (light == 0) return offRate;
  if (offRate <= 0 || onRate <= 0) return onRate;
  return offRate + std::max(onRate - offRate, 0.0f) * lightDuty(light) / std::max(avgDuty, 1.0f);
}

}  // namespace BatteryEstimate
