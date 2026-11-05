#include <GlobalReadingStats.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

namespace {
constexpr char LEGACY[] = "/.crossdink/global_stats.bin";
constexpr char DINK[] = "/.crossdink/global_stats_dink.bin";

// A bare v3 payload as older firmware writes it.
void writeLegacy(const uint32_t sessions) {
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", LEGACY, file));
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
  void SetUp() override { Storage.reset(); }
};

TEST_F(GlobalStatsSlotsTest, MigratesLegacyFileAndLeavesItForOlderFirmware) {
  writeLegacy(7);
  GlobalReadingStats stats = GlobalReadingStats::load();
  EXPECT_EQ(stats.totalSessions, 7U);

  stats.totalSessions = 8;
  stats.save();
  EXPECT_EQ(fileSize(DINK), GlobalReadingStats::CURRENT_FILE_SIZE + 8);
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
