#include <gtest/gtest.h>

#include "TouchActionButtons.h"

namespace {
constexpr int padding = TouchActionButtons::kDefaultGap / 2;

// Two default-size buttons stacked in a 440x128 container at (20, 656), as
// TouchActionButtons::vertical() lays them out: y 656-711 and 728-783.
TouchActionButtons::Layout twoActions() {
  TouchActionButtons::Layout layout;
  layout.count = 2;
  layout.buttons[0] = Rect{20, 656, 440, TouchActionButtons::kDefaultHeight};
  layout.buttons[1] = Rect{20, 728, 440, TouchActionButtons::kDefaultHeight};
  return layout;
}
}  // namespace

TEST(TouchActionButtons, AcceptsStickyLaterContactWithExplicitPadding) {
  const auto actions = twoActions();
  // Recorded from Sticky: a valid tap just below the second button's visible bottom at y=784.
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 207, 789), -1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 207, 789, padding), 1);
}

TEST(TouchActionButtons, PaddingStaysBoundedAndDoesNotMergeActions) {
  const auto actions = twoActions();
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 719, padding), 0);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 720, padding), 1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 791, padding), 1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 792, padding), -1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 11, 756, padding), -1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 12, 756, padding), 1);
  EXPECT_EQ(TouchActionButtons::indexAt(actions, 240, 650, -5), -1);
}

TEST(TouchActionButtons, PaddingDoesNotActivateAnEmptyButton) {
  TouchActionButtons::Layout empty;
  empty.count = 2;
  EXPECT_EQ(TouchActionButtons::indexAt(empty, 0, 0, padding), -1);
}
