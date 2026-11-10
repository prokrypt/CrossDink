#pragma once

#include <cmath>
#include <cstdint>

// Frontlight ramps (toggle/sleep/Light Timeout fades, flash duck) step evenly in perceived
// lightness, not in percent. Light follows percent^1.6554 (FrontlightGamma) and the eye follows
// light^(1/3) (CIE L*), so a ramp linear in percent spent most of a fade-out near full and then
// dropped the last ~30% of what the eye sees in the final tenth, in two or three 10 ms steps.
// Works on any unit proportional to percent (1/256 %, or 1/100 % of the user level).
inline uint16_t lightRampLerp(const uint16_t from, const uint16_t to, float f) {
  if (f <= 0.0f) return from;
  if (f >= 1.0f) return to;
  constexpr float k = 1.6554f / 3.0f;
  const float a = std::pow(static_cast<float>(from), k), b = std::pow(static_cast<float>(to), k);
  return static_cast<uint16_t>(std::lround(std::pow(a + (b - a) * f, 1.0f / k)));
}
