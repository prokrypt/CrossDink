#include <gtest/gtest.h>

#include "TransferLightPulse.h"

TEST(TransferLightPulse, LitLightDipsAndNeverExceedsTheUserLevel) {
  for (int base = 1; base <= 100; base++) {
    const uint8_t lo = pulseLow(base), hi = pulseHigh(base, 25);
    EXPECT_EQ(hi, base) << base;
    EXPECT_LT(lo, base) << base;
    EXPECT_LE(4 * lo, base) << base;
  }
  EXPECT_EQ(pulseLow(0), 0);
  EXPECT_EQ(pulseHigh(0, 25), 25);
}
