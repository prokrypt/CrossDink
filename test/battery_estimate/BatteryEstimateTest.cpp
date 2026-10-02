#include <gtest/gtest.h>

#include "src/util/BatteryEstimate.h"

using BatteryEstimate::lightScaledRate;

TEST(BatteryEstimate, BrighterLightDrainsFaster) {
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 50.0f, 50), 3.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 50.0f, 100), 4.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 50.0f, 0), 2.0f);
}

// Device log 10/1: Wi-Fi on, light off 3.08 %/h; light on (avg 11.6%) 1.73 %/h.
TEST(BatteryEstimate, LowerLightOnDrainNeverShortensDrainWithBrightness) {
  EXPECT_FLOAT_EQ(lightScaledRate(3.08f, 1.73f, 11.6f, 1), 3.08f);
  EXPECT_FLOAT_EQ(lightScaledRate(3.08f, 1.73f, 11.6f, 100), 3.08f);
}

TEST(BatteryEstimate, MissingStateFallsBack) {
  EXPECT_FLOAT_EQ(lightScaledRate(0.0f, 3.0f, 50.0f, 80), 3.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 0.0f, 50.0f, 80), 0.0f);
}
