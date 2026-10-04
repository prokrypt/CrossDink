#include <gtest/gtest.h>

#include "TransferLightPulse.h"

TEST(TransferLightPulse, EveryLevelHasAVisibleSwing) {
  for (int base = 1; base <= 100; base++) {
    const uint8_t lo = pulseLow(base), hi = pulseHigh(base, 25, 10);
    EXPECT_LE(lo, base) << base;
    EXPECT_GE(hi, base) << base;
    EXPECT_GE(hi, 25) << base;
    EXPECT_LE(hi, 100) << base;
    EXPECT_GE(hi - lo, 18) << base;
    EXPECT_GE(hi, 3 * lo) << base;  // at least 3x light ratio (above ~1.7x by eye)
  }
  EXPECT_EQ(pulseLow(0), 0);
  EXPECT_EQ(pulseHigh(0, 25, 10), 10);
}
