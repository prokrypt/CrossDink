#include <gtest/gtest.h>

#include "util/TouchNoiseMonitor.h"

namespace {

TEST(TouchNoiseMonitorTest, WakesWithoutGestureLogOncePerMinute) {
  TouchNoiseMonitor m;
  EXPECT_FALSE(m.update(1000, 0, false, false));
  EXPECT_FALSE(m.update(3000, 60, false, true));
  // 96 wakes in 5 s with no gesture, as in the charger log.
  ASSERT_TRUE(m.update(6000, 96, false, false));
  EXPECT_EQ(m.wakes, 96u);
  EXPECT_EQ(m.contacts, 1u);
  EXPECT_EQ(m.spanMs, 5000u);
  // Still noisy, but rate-limited until a minute has passed.
  EXPECT_FALSE(m.update(11000, 192, false, false));
  EXPECT_TRUE(m.update(66000, 2000, false, false));
}

TEST(TouchNoiseMonitorTest, GestureOrLowRateIsNotNoise) {
  TouchNoiseMonitor m;
  m.update(0, 0, false, false);
  EXPECT_FALSE(m.update(2000, 75, true, true));  // a swipe
  EXPECT_FALSE(m.update(5000, 150, false, false));
  // 49 wakes in 5 s, then 99 wakes over a 10 s idle tick: below 10 wakes/s.
  EXPECT_FALSE(m.update(10000, 199, false, false));
  EXPECT_FALSE(m.update(20000, 298, false, false));
}

}  // namespace
