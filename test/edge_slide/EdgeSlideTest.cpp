#include <gtest/gtest.h>

#include "src/util/EdgeSlide.h"
#include "src/util/SwipeAdjustment.h"

TEST(EdgeSlide, EdgeLightAdjustmentStepsByOnePoint) {
  EXPECT_EQ(SwipeAdjustment::edgeAmount(59, 800), 0);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(60, 800), 1);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(74, 800), 1);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(75, 800), 2);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(429, 800), 25);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(799, 800), 50);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(1200, 800), 50);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(60, 480), 1);
  EXPECT_EQ(SwipeAdjustment::edgeAmount(479, 480), 50);
}

TEST(EdgeSlide, LiveLightTargetFollowsFingerBackFromLimit) {
  EXPECT_EQ(SwipeAdjustment::targetValue(95, true, 20), 100);
  EXPECT_EQ(SwipeAdjustment::targetValue(95, true, 5), 100);
  EXPECT_EQ(SwipeAdjustment::targetValue(95, true, 0), 95);
  EXPECT_EQ(SwipeAdjustment::targetValue(10, false, 20), 0);
  EXPECT_EQ(SwipeAdjustment::targetValue(10, false, 5), 5);
}

TEST(EdgeSlide, RecognizesBothSidesAndDirections) {
  EXPECT_EQ(EdgeSlide::directionFor(10, 500, 18, 350, 480, 800), EdgeSlide::Direction::LeftUp);
  EXPECT_EQ(EdgeSlide::directionFor(10, 350, 18, 500, 480, 800), EdgeSlide::Direction::LeftDown);
  EXPECT_EQ(EdgeSlide::directionFor(470, 500, 465, 350, 480, 800), EdgeSlide::Direction::RightUp);
  EXPECT_EQ(EdgeSlide::directionFor(470, 350, 465, 500, 480, 800), EdgeSlide::Direction::RightDown);
}

TEST(EdgeSlide, LeavesCenterAndDiagonalGesturesAlone) {
  EXPECT_EQ(EdgeSlide::directionFor(100, 500, 100, 350, 480, 800), EdgeSlide::Direction::None);
  EXPECT_EQ(EdgeSlide::directionFor(10, 500, 80, 350, 480, 800), EdgeSlide::Direction::None);
  EXPECT_EQ(EdgeSlide::directionFor(10, 500, 30, 460, 480, 800), EdgeSlide::Direction::None);
  EXPECT_EQ(EdgeSlide::directionFor(10, 500, 35, 450, 480, 800), EdgeSlide::Direction::None);
  EXPECT_EQ(EdgeSlide::directionFor(10, 400, 60, 340, 800, 480), EdgeSlide::Direction::None);
  EXPECT_EQ(EdgeSlide::directionFor(790, 500, 720, 350, 800, 800), EdgeSlide::Direction::None);
}

TEST(EdgeSlide, LiveAmountHasNoDeadZoneAfterRecognition) {
  // Matches edgeAmount() from the recognition point onward.
  EXPECT_EQ(SwipeAdjustment::liveAmount(60, 800), 1);
  EXPECT_EQ(SwipeAdjustment::liveAmount(75, 800), 2);
  EXPECT_EQ(SwipeAdjustment::liveAmount(429, 800), 25);
  EXPECT_EQ(SwipeAdjustment::liveAmount(1200, 800), 50);
  // Below the recognition point it keeps stepping by one point instead of holding.
  EXPECT_EQ(SwipeAdjustment::liveAmount(59, 800), 0);
  EXPECT_EQ(SwipeAdjustment::liveAmount(46, 800), 0);
  EXPECT_EQ(SwipeAdjustment::liveAmount(45, 800), -1);
  EXPECT_LT(SwipeAdjustment::liveAmount(0, 800), 0);
  for (int d = -500; d < 800; ++d) {
    const int step = SwipeAdjustment::liveAmount(d + 1, 800) - SwipeAdjustment::liveAmount(d, 800);
    EXPECT_TRUE(step == 0 || step == 1) << d;
  }
  // Start at 20%, slide down to decrease, then back up past the touch-down point.
  EXPECT_EQ(SwipeAdjustment::targetValue(20, false, SwipeAdjustment::liveAmount(429, 800)), 0);
  EXPECT_GT(SwipeAdjustment::targetValue(20, false, SwipeAdjustment::liveAmount(0, 800)), 20);
  EXPECT_EQ(SwipeAdjustment::targetValue(20, false, SwipeAdjustment::liveAmount(-429, 800)), 52);
}
