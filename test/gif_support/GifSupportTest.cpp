#include <GifImageLayout.h>
#include <gtest/gtest.h>

namespace {
TEST(GifSupport, TallAndWideCoversStayWithinTheirTarget) {
  constexpr std::pair<int, int> dimensionsCases[] = {{1, 3072}, {3072, 1}, {640, 480}};
  constexpr std::pair<int, int> targets[] = {{480, 800}, {800, 480}};
  for (const auto& dimensions : dimensionsCases) {
    for (const auto& target : targets) {
      GifCommon::ImageLayout layout;
      ASSERT_TRUE(
          GifCommon::calculateLayout(dimensions.first, dimensions.second, target.first, target.second, true, layout));
      EXPECT_EQ(layout.width, target.first);
      EXPECT_EQ(layout.height, target.second);
      EXPECT_GE(layout.sourceX, 0);
      EXPECT_GE(layout.sourceY, 0);
      EXPECT_LE(layout.sourceX + layout.sourceWidth, dimensions.first);
      EXPECT_LE(layout.sourceY + layout.sourceHeight, dimensions.second);
    }
  }
}

TEST(GifSupport, FitRetainsTheWholeSource) {
  GifCommon::ImageLayout layout;
  ASSERT_TRUE(GifCommon::calculateLayout(640, 480, 240, 400, false, layout));
  EXPECT_EQ(layout.width, 240);
  EXPECT_EQ(layout.height, 180);
  EXPECT_EQ(layout.sourceWidth, 640);
  EXPECT_EQ(layout.sourceHeight, 480);
}

TEST(GifSupport, RejectsInvalidOrOversizedTargets) {
  GifCommon::ImageLayout layout;
  EXPECT_FALSE(GifCommon::calculateLayout(1, 1, 0, 800, true, layout));
  EXPECT_FALSE(GifCommon::calculateLayout(1, 1, 480, -1, true, layout));
  EXPECT_FALSE(GifCommon::calculateLayout(1, 1, 480, 32767, true, layout));
}
}  // namespace
