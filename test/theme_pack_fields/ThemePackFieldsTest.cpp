#include <gtest/gtest.h>

#include "components/ThemePackFields.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/Lyra3CoversTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/lyra/LyraTheme.h"
#include "components/themes/minimal/MinimalTheme.h"
#include "components/themes/roundedraff/RoundedRaffTheme.h"

namespace TPF = ThemePackFields;

// themed() lives in ThemePack.cpp, which this test does not link.
const ThemeMetrics& themed(const ThemeMetrics& builtIn) { return builtIn; }

TEST(ThemePackFields, EveryBuiltInThemeFitsTheRanges) {
  const ThemeMetrics* themes[] = {&BaseMetrics::values,         &LyraMetrics::values,    &Lyra3CoversMetrics::values,
                                  &LyraCarouselMetrics::values, &MinimalMetrics::values, &DashboardMetrics::values,
                                  &RoundedRaffMetrics::values};
  for (const ThemeMetrics* theme : themes) {
    for (int i = 0; i < TPF::count(); ++i) {
      const auto& f = TPF::at(i);
      const int32_t v = TPF::read(*theme, i);
      EXPECT_GE(v, f.min) << f.key;
      EXPECT_LE(v, f.max) << f.key;
    }
  }
}

TEST(ThemePackFields, KeysAreUniqueAndFound) {
  for (int i = 0; i < TPF::count(); ++i) {
    EXPECT_EQ(TPF::find(TPF::at(i).key), i) << TPF::at(i).key;
  }
  EXPECT_EQ(TPF::find("homeRecentBooksCount"), -1);  // array bounds in theme code
  EXPECT_EQ(TPF::find("buttonHintsHeight"), -1);
  EXPECT_EQ(TPF::find("nope"), -1);
}

TEST(ThemePackFields, ParseClampsAndChecksTypes) {
  JsonDocument doc;
  ASSERT_FALSE(deserializeJson(doc, R"({"row":40,"huge":1e30,"neg":-500,"bold":true,"text":"x","ratio":0.2})"));
  const int row = TPF::find("listRowHeight");
  const int bold = TPF::find("listTitleBold");
  const int ratio = TPF::find("popupTopOffsetRatio");
  int32_t v = 0;
  EXPECT_EQ(TPF::parse(row, doc["row"], v), TPF::Parse::Ok);
  EXPECT_EQ(v, 40);
  EXPECT_EQ(TPF::parse(row, doc["huge"], v), TPF::Parse::Clamped);
  EXPECT_EQ(v, TPF::at(row).max);
  EXPECT_EQ(TPF::parse(row, doc["neg"], v), TPF::Parse::Clamped);
  EXPECT_EQ(v, TPF::at(row).min);
  EXPECT_EQ(TPF::parse(row, doc["text"], v), TPF::Parse::Bad);
  EXPECT_EQ(TPF::parse(row, doc["bold"], v), TPF::Parse::Bad);
  EXPECT_EQ(TPF::parse(bold, doc["row"], v), TPF::Parse::Bad);
  EXPECT_EQ(TPF::parse(bold, doc["bold"], v), TPF::Parse::Ok);
  EXPECT_EQ(v, 1);
  EXPECT_EQ(TPF::parse(ratio, doc["ratio"], v), TPF::Parse::Ok);
  EXPECT_EQ(v, 200);
}

TEST(ThemePackFields, ApplyWritesOnlyTheField) {
  ThemeMetrics m = LyraMetrics::values;
  TPF::apply(m, TPF::find("listRowHeight"), 48);
  TPF::apply(m, TPF::find("listTitleBold"), 1);
  TPF::apply(m, TPF::find("popupTopOffsetRatio"), 250);
  TPF::apply(m, TPF::find("tabBarAppearance"), 1);
  EXPECT_EQ(m.listRowHeight, 48);
  EXPECT_TRUE(m.listTitleBold);
  EXPECT_FLOAT_EQ(m.popupTopOffsetRatio, 0.25f);
  EXPECT_EQ(m.tabBarAppearance, ThemeTabBarAppearance::BorderedText);
  EXPECT_EQ(m.listWithSubtitleRowHeight, LyraMetrics::values.listWithSubtitleRowHeight);
  EXPECT_EQ(m.headerHeight, LyraMetrics::values.headerHeight);
}
