#include <gtest/gtest.h>

#include "src/util/BatteryEstimate.h"

using BatteryEstimate::lightDuty;
using BatteryEstimate::lightScaledRate;

// FrontlightManager's GAMMA_TABLE scaled to the X4 Pro's 10-bit duty.
TEST(BatteryEstimate, DutyMatchesFrontlightCurve) {
  EXPECT_EQ(lightDuty(0), 0);
  EXPECT_EQ(lightDuty(1), 1);
  EXPECT_EQ(lightDuty(10), 23);
  EXPECT_EQ(lightDuty(50), 325);
  EXPECT_EQ(lightDuty(76), 650);
  EXPECT_EQ(lightDuty(100), 1023);
}

TEST(BatteryEstimate, LightShareScalesWithDuty) {
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 325.0f, 50), 3.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 325.0f, 100), 2.0f + 1023.0f / 325.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 23.0f, 1), 2.0f + 1.0f / 23.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 325.0f, 0), 2.0f);
}

// Device log 10/1: Wi-Fi on, light off 3.08 %/h; light on (avg 11.6%) 1.73 %/h.
TEST(BatteryEstimate, LowerLightOnDrainNeverShortensDrainWithBrightness) {
  EXPECT_FLOAT_EQ(lightScaledRate(3.08f, 1.73f, 28.0f, 1), 3.08f);
  EXPECT_FLOAT_EQ(lightScaledRate(3.08f, 1.73f, 28.0f, 100), 3.08f);
}

TEST(BatteryEstimate, MissingStateFallsBack) {
  EXPECT_FLOAT_EQ(lightScaledRate(0.0f, 3.0f, 325.0f, 80), 3.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 0.0f, 325.0f, 80), 0.0f);
}
