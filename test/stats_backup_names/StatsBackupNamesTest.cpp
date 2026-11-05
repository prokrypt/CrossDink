#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "src/activities/reader/StatsBackupNames.h"

namespace {

// Mirrors pruneBackups' selection: returns the names that survive.
std::vector<std::string> survivors(const std::vector<std::string>& names, int keep) {
  std::vector<std::string> out;
  for (const bool daily : {true, false}) {
    std::vector<std::string> set;
    for (const auto& n : names)
      if (isDailyStatsBackupName(n.c_str()) == daily) set.push_back(n);
    std::sort(set.begin(), set.end(),
              [](const std::string& a, const std::string& b) { return statsBackupOlder(a.c_str(), b.c_str()); });
    const size_t drop = set.size() > static_cast<size_t>(keep) ? set.size() - keep : 0;
    out.insert(out.end(), set.begin() + drop, set.end());
  }
  return out;
}

bool has(const std::vector<std::string>& v, const char* n) { return std::find(v.begin(), v.end(), n) != v.end(); }

}  // namespace

TEST(StatsBackupNames, ClassifiesNames) {
  EXPECT_TRUE(isDailyStatsBackupName("stats_2026-10-03.bin"));
  EXPECT_FALSE(isDailyStatsBackupName("stats_2026-10-03_1200.bin"));
  EXPECT_FALSE(isDailyStatsBackupName("stats_backup_001.bin"));
}

TEST(StatsBackupNames, ManualBackupsDoNotPushOutDaily) {
  std::vector<std::string> names;
  for (int d = 1; d <= 7; ++d) names.push_back("stats_2026-10-0" + std::to_string(d) + ".bin");
  for (int m = 0; m < 9; ++m) names.push_back("stats_2026-10-08_120" + std::to_string(m) + ".bin");
  const auto kept = survivors(names, 7);
  EXPECT_TRUE(has(kept, "stats_2026-10-01.bin"));
  EXPECT_TRUE(has(kept, "stats_2026-10-07.bin"));
  EXPECT_FALSE(has(kept, "stats_2026-10-08_1200.bin"));
  EXPECT_TRUE(has(kept, "stats_2026-10-08_1208.bin"));
  EXPECT_EQ(kept.size(), 14u);
}

TEST(StatsBackupNames, ClockslessNumberedSortNumericallyAndNewestSurvives) {
  std::vector<std::string> names = {"stats_backup_998.bin", "stats_backup_999.bin", "stats_backup_1000.bin",
                                    "stats_2026-10-03.bin"};
  const auto kept = survivors(names, 1);
  EXPECT_TRUE(has(kept, "stats_backup_1000.bin"));
  EXPECT_FALSE(has(kept, "stats_backup_999.bin"));
  EXPECT_TRUE(has(kept, "stats_2026-10-03.bin"));
}
