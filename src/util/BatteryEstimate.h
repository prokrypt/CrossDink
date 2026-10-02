#pragma once

#include <algorithm>
#include <cstdint>

namespace BatteryEstimate {

// Awake drain at `light` % brightness from the logged drain with the light off
// and on (same unit, 0 = not enough data) and the light-on stretches' average
// brightness. The light only adds drain: when the light-on stretches drained
// less (the light-off ones held heavier work), it adds nothing rather than
// making a brighter light last longer.
inline float lightScaledRate(const float offRate, const float onRate, const float avgLight, const uint8_t light) {
  if (light == 0) return offRate;
  if (offRate <= 0 || onRate <= 0) return onRate;
  return offRate + std::max(onRate - offRate, 0.0f) * light / std::max(avgLight, 1.0f);
}

}  // namespace BatteryEstimate
