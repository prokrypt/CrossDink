#include <gtest/gtest.h>

#include <cmath>

#include "include/LightRamp.h"

TEST(LightRamp, EndsExact) {
  EXPECT_EQ(lightRampLerp(7680, 0, 0.0f), 7680);
  EXPECT_EQ(lightRampLerp(7680, 0, 1.0f), 0);
  EXPECT_EQ(lightRampLerp(256, 0, 1.0f), 0);
  EXPECT_EQ(lightRampLerp(2000, 10000, 1.0f), 10000);
}

// A 30% fade-out in 10 ms steps over 300 ms: no step may change perceived lightness by more
// than ~1/15 of the range (the linear-in-% ramp's last step was over 20%).
TEST(LightRamp, EvenPerceivedSteps) {
  auto perceived = [](uint16_t q8) { return std::cbrt(std::pow(q8 / 25600.0, 1.6554)); };
  const double range = perceived(7680);
  double prev = range;
  for (int i = 1; i <= 30; ++i) {
    const double now = perceived(lightRampLerp(7680, 0, i / 30.0f));
    EXPECT_LE(prev - now, range / 15) << "step " << i;
    EXPECT_LE(now, prev) << "step " << i;
    prev = now;
  }
}
