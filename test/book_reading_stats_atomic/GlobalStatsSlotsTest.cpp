#include <GlobalReadingStats.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

namespace {
constexpr char LEGACY[] = "/.crossdink/global_stats.bin";
constexpr char DINK[] = "/.crossdink/global_stats_dink.bin";
constexpr char LOCAL[] = "/.crossdink/global_stats_crossink-000000000000.bin";
constexpr char ROOT_CLAIM[] = "/.crossdink/stats_origin.bin";
constexpr char OTHER[] = "crossink-0123456789ab";

void writeClaim(const char* path, const char* id) {
  SyncFiles::Writer writer(path);
  char stored[32]{};
  snprintf(stored, sizeof(stored), "%s", id);
  ASSERT_TRUE(writer.open() && writer.pod(uint8_t{1}) && writer.write(stored, sizeof(stored)) && writer.commit());
}

// A bare v3 payload as older firmware writes it.
void writeLegacy(const uint32_t sessions, const char* path = LEGACY) {
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
  uint8_t data[GlobalReadingStats::CURRENT_FILE_SIZE] = {GlobalReadingStats::CURRENT_FILE_VERSION,
                                                         static_cast<uint8_t>(sessions)};
  file.write(data, sizeof(data));
  file.close();
}

size_t fileSize(const char* path) {
  HalFile file;
  if (!Storage.openFileForRead("TEST", path, file)) return 0;
  const size_t size = file.fileSize();
  file.close();
  return size;
}
}  // namespace

class GlobalStatsSlotsTest : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    StatsStorage::forget();
  }
};

TEST_F(GlobalStatsSlotsTest, MigratesLegacyFileAndLeavesItForOlderFirmware) {
  writeLegacy(7);
  GlobalReadingStats stats = GlobalReadingStats::load();
  EXPECT_EQ(stats.totalSessions, 7U);

  stats.totalSessions = 8;
  stats.save();
  EXPECT_EQ(fileSize(LOCAL), GlobalReadingStats::CURRENT_FILE_SIZE + 8);
  EXPECT_TRUE(Storage.exists(ROOT_CLAIM));
  EXPECT_EQ(fileSize(LEGACY), GlobalReadingStats::CURRENT_FILE_SIZE);  // untouched, older firmware still reads it
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 8U);

  std::array<uint8_t, GlobalReadingStats::CURRENT_FILE_SIZE> payload{};
  EXPECT_EQ(GlobalReadingStats::readLocalFile(payload), GlobalReadingStats::CURRENT_FILE_SIZE);
  EXPECT_EQ(payload[1], 8U);
}

TEST_F(GlobalStatsSlotsTest, ResetClearsLegacyFileToo) {
  writeLegacy(7);
  GlobalReadingStats stats = GlobalReadingStats::load();
  stats.totalSessions = 9;
  stats.save();
  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_FALSE(Storage.exists(LEGACY));
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 0U);
}

TEST_F(GlobalStatsSlotsTest, OlderCrossDinkPairWinsOverOlderFirmwareFile) {
  writeLegacy(7);
  writeLegacy(8, DINK);  // a bare payload stands in for the dink pair
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 8U);
  GlobalReadingStats stats;
  stats.totalSessions = 9;
  stats.save();
  EXPECT_TRUE(Storage.exists(DINK));  // left for a rollback
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 9U);
}

TEST_F(GlobalStatsSlotsTest, AnotherReadersStatsAreNeitherLoadedNorReset) {
  writeLegacy(7, DINK);
  writeClaim(ROOT_CLAIM, OTHER);
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 0U);
  GlobalReadingStats stats;
  stats.totalSessions = 1;
  stats.save();
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 1U);
  EXPECT_TRUE(GlobalReadingStats::resetLocal());
  EXPECT_TRUE(Storage.exists(DINK));
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 0U);
}

TEST_F(GlobalStatsSlotsTest, CrossInksTwinFileFollowsTheTwinClaim) {
  writeLegacy(7, DINK);
  writeLegacy(5);  // CrossInk's /.crosspoint/global_stats.bin showing through
  Storage.markTwinOnly(LEGACY);
  writeClaim("/.crosspoint/stats_origin.bin", OTHER);
  Storage.remove(DINK);
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 0U);
  writeLegacy(7, DINK);  // this reader's own pair is native: the twin claim says nothing about it
  StatsStorage::forget();
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 7U);
}

TEST_F(GlobalStatsSlotsTest, UnreadableRootClaimRefusesTheFirstSave) {
  writeLegacy(7, DINK);
  HalFile claim;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", ROOT_CLAIM, claim));
  uint8_t data[37] = {1};  // complete, CRC wrong
  claim.write(data, sizeof(data));
  claim.close();
  EXPECT_EQ(GlobalReadingStats::load().totalSessions, 0U);
  GlobalReadingStats stats;
  stats.totalSessions = 1;
  stats.save();
  EXPECT_FALSE(Storage.exists(LOCAL));
  EXPECT_FALSE(GlobalReadingStats::resetLocal());
  EXPECT_TRUE(Storage.exists(DINK));
}
