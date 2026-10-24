#pragma once

#include <cstdint>

// Ghost Busters' quiet time from the user's own pace (ActivityManager).
namespace GhostBusterPace {

constexpr uint32_t MAX_QUIET_MS = 8000;
constexpr uint32_t QUIET_STEP_MS = 250;

// gaps: recent gaps between input starts. A gap in [quiet, quiet + busyMs) is a
// press that would have landed on a buster run. Returns the earliest quiet time
// from floorMs up to MAX_QUIET_MS at which at most 1 in 20 gaps would, else the
// one with the fewest such gaps.
inline uint32_t quietMs(const uint16_t* gaps, const uint8_t count, const uint32_t busyMs, const uint32_t floorMs,
                        uint8_t* hitsOut = nullptr) {
  uint32_t best = MAX_QUIET_MS;
  uint8_t bestHits = UINT8_MAX;
  for (uint32_t quiet = floorMs; quiet <= MAX_QUIET_MS; quiet += QUIET_STEP_MS) {
    uint8_t hits = 0;
    for (uint8_t i = 0; i < count; ++i) {
      if (gaps[i] >= quiet && gaps[i] < quiet + busyMs) hits++;
    }
    if (hits < bestHits) {
      best = quiet;
      bestHits = hits;
    }
    if (hits * 20 <= count) break;
  }
  if (hitsOut) *hitsOut = bestHits;
  return best;
}

}  // namespace GhostBusterPace
