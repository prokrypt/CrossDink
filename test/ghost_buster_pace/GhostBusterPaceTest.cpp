#include <gtest/gtest.h>

#include <vector>

#include "GhostBusterPace.h"

namespace {
uint32_t quiet(const std::vector<uint16_t>& gaps, const uint32_t busyMs = 1800, uint8_t* hits = nullptr) {
  return GhostBusterPace::quietMs(gaps.data(), static_cast<uint8_t>(gaps.size()), busyMs, 1000, hits);
}
}  // namespace

TEST(GhostBusterPace, FastPaceKeepsTheFloor) {
  // Quick navigation and long pauses: nothing lands in [1000, 2800).
  EXPECT_EQ(quiet({200, 300, 250, 400, 350, 300, 9000, 12000}), 1000u);
}

TEST(GhostBusterPace, WaitsPastAPauseCluster) {
  // Presses after ~1.5 s pauses: start busting after them (2600 + 1800 > 4300).
  EXPECT_EQ(quiet({1500, 1400, 1600, 1550, 1450, 1500, 1520, 1480, 4400, 6000}), 1750u);
}

TEST(GhostBusterPace, AllowsOneInTwenty) {
  std::vector<uint16_t> gaps(20, 300);
  gaps[0] = 1200;  // one gap of twenty in the floor's window is allowed
  EXPECT_EQ(quiet(gaps), 1000u);
}

TEST(GhostBusterPace, FallsBackToFewestHits) {
  // Gaps every 0.4 s from 1 s to 9.8 s: every 1.8 s window holds 4 or 5 of
  // them, so no quiet time is clean; the earliest window with 4 starts at 1.5 s.
  std::vector<uint16_t> gaps;
  for (uint16_t g = 1000; g <= 9800; g += 400) gaps.push_back(g);
  uint8_t hits = 0;
  const uint32_t q = quiet(gaps, 1800, &hits);
  EXPECT_EQ(q, 1500u);
  EXPECT_EQ(hits, 4u);
}
