#pragma once

// The firmware HAL on device; the simulator library's host model of the same
// class in simulator builds (crossdink-simulator src/HalFrontlight.h).
#include <HalFrontlight.h>

// Light toggle and sleep fades; the simulator's model has no fade, so it snaps.
// Dim level (Light Timeout, flash duck) in 1/100 %; the simulator's model takes whole % and snaps.
#ifdef SIMULATOR
inline void frontlightSetOnFaded(const bool on) { Frontlight.setOn(on); }
inline void frontlightFadeOutForSleep() {}
constexpr uint16_t FRONTLIGHT_DIM_FULL = 10000;
inline uint16_t frontlightDim() { return Frontlight.idleDimPercent() * 100; }
inline void frontlightSetDim(const uint16_t level, uint32_t = 0) { Frontlight.setIdleDim(level / 100); }
#else
inline void frontlightSetOnFaded(const bool on) { Frontlight.setOn(on, true); }
inline void frontlightFadeOutForSleep() { Frontlight.fadeOutForSleep(); }
constexpr uint16_t FRONTLIGHT_DIM_FULL = HalFrontlight::DIM_FULL;
inline uint16_t frontlightDim() { return Frontlight.idleDimFine(); }
inline void frontlightSetDim(const uint16_t level, const uint32_t rampMs = 0) {
  Frontlight.setIdleDimFine(level, rampMs);
}
#endif
