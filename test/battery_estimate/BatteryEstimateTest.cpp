#include <gtest/gtest.h>

#include "src/util/BatteryEstimate.h"

using BatteryEstimate::ledSlope;
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

// Device log 10/3 (.67): Wi-Fi on, light off rose 0.58% (no rate); light on (duty 20.4) 0.137 c%/s.
// Wi-Fi off: 0.0125 light off, 0.0509 light on (duty 78.9). The estimate stayed at 10h 15m at
// any brightness; the Wi-Fi-off slope now gives the light's share.
TEST(BatteryEstimate, OtherWifiStateGivesLightShare) {
  const float slope = ledSlope(0.01252f, 0.05091f, 78.9f);
  EXPECT_FLOAT_EQ(slope, (0.05091f - 0.01252f) / 78.9f);
  const float off = 0.13703f - slope * 20.4f;
  EXPECT_FLOAT_EQ(lightScaledRate(0.0f, 0.13703f, 20.4f, 0, slope), off);
  EXPECT_FLOAT_EQ(lightScaledRate(0.0f, 0.13703f, 20.4f, 100, slope), off + slope * 1023);
  EXPECT_LT(lightScaledRate(0.0f, 0.13703f, 20.4f, 15, slope), lightScaledRate(0.0f, 0.13703f, 20.4f, 50, slope));
  // A pair that drained less with the light on takes the other slope too.
  EXPECT_FLOAT_EQ(lightScaledRate(3.08f, 1.73f, 28.0f, 100, 0.001f), 3.08f + 0.001f * 1023);
  // A missing light-on rate adds the other slope's share.
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 0.0f, 325.0f, 50, 0.001f), 2.0f + 0.001f * 325);
  // No usable slope, or this pair can tell: unchanged.
  EXPECT_LT(ledSlope(0.0f, 0.13703f, 20.4f), 0.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(0.0f, 3.0f, 325.0f, 80, -1.0f), 3.0f);
  EXPECT_FLOAT_EQ(lightScaledRate(2.0f, 3.0f, 325.0f, 100, 0.5f), 2.0f + 1023.0f / 325.0f);
}
