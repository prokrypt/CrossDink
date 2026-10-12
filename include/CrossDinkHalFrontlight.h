#pragma once

// The firmware HAL on device; the simulator library's host model of the same
// class in simulator builds (crossdink-simulator src/HalFrontlight.h).
#include <HalFrontlight.h>

// Light toggle and sleep fades; the simulator's model has no fade, so it snaps.
#ifdef SIMULATOR
inline void frontlightSetOnFaded(const bool on) { Frontlight.setOn(on); }
inline void frontlightFadeOutForSleep() {}
#else
inline void frontlightSetOnFaded(const bool on) { Frontlight.setOn(on, true); }
inline void frontlightFadeOutForSleep() { Frontlight.fadeOutForSleep(); }
#endif
