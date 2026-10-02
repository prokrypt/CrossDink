#pragma once

#include <Knobs.h>

#include <algorithm>

namespace SwipeAdjustment {

// Travel before an edge slide, a two-finger swipe or a light step counts
// (Goodies > Knobs swipeDeadPx / swipeDeadPct), shared by all of them.
inline int deadZone(const int axisSize) {
  return std::max<int>(KNOBS.swipeDeadPx, axisSize * KNOBS.swipeDeadPct / 100);
}

// Light points per swipe step: a full-axis swipe covers about
// KNOBS.swipeFullAxisPts of the 0-100 range.
inline int stepNumerator() { return 2 * KNOBS.swipeFullAxisPts - 1; }

// Edge slides and two-finger swipes adjust the frontlight in 1-point steps:
// the first step lands as soon as the gesture is recognized and a full-axis
// swipe covers about half of the 0-100 range.
inline int edgeAmount(const int distance, const int axisSize) {
  if (axisSize < 2) return 0;
  const int minimumDistance = deadZone(axisSize);
  if (distance < minimumDistance) return 0;
  const int maximumDistance = axisSize - 1;
  const int travel = std::max(1, maximumDistance - minimumDistance);
  return 1 + (std::min(distance, maximumDistance) - minimumDistance) * stepNumerator() / (2 * travel);
}

// Once a live light swipe is recognized, the dead zone is behind it: the value
// tracks the finger on the same 1-point scale as edgeAmount() in both
// directions, so reversing back through the touch-down point keeps stepping
// instead of holding at the starting value. displacement is signed along the
// swipe's starting direction.
inline int liveAmount(const int displacement, const int axisSize) {
  if (axisSize < 2) return 0;
  const int minimumDistance = deadZone(axisSize);
  const int maximumDistance = axisSize - 1;
  const int travel = std::max(1, maximumDistance - minimumDistance);
  const int numerator = (std::min(displacement, maximumDistance) - minimumDistance) * stepNumerator();
  const int denominator = 2 * travel;
  int steps = numerator / denominator;
  if (numerator % denominator != 0 && numerator < 0) --steps;  // Floor so steps stay evenly spaced below zero.
  return 1 + steps;
}

inline int targetValue(const int initialValue, const bool increase, const int adjustment) {
  return std::clamp(initialValue + (increase ? adjustment : -adjustment), 0, 100);
}

}  // namespace SwipeAdjustment
