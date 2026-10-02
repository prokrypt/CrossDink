#include <BookReadingStats.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>

namespace {
constexpr char CACHE_PATH[] = "/cache/book";
const std::string statsPath = std::string(CACHE_PATH) + "/stats_v5.bin";
const std::string backupPath = statsPath + ".bak";
const std::string previousStatsPath = std::string(CACHE_PATH) + "/stats_v4.bin";

BookReadingStats statsWithSeconds(const uint32_t seconds) {
  BookReadingStats stats;
  stats.totalReadingSeconds = seconds;
  return stats;
}

size_t fileSize(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("TEST", path, file)) return 0;
  const size_t size = file.fileSize();
  file.close();
  return size;
}

// Flips one payload byte, as a torn in-place write would.
void corrupt(const std::string& path) {
  HalFile file = Storage.open(path.c_str(), O_RDWR);
  uint8_t byte = 0;
  file.seekSet(3);
  file.read(&byte, 1);
  byte ^= 0xFF;
  file.seekSet(3);
  file.write(&byte, 1);
  file.close();
}
}  // namespace

class BookReadingStatsAtomicTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(BookReadingStatsAtomicTest, SavesAlternateBetweenSlots) {
  EXPECT_TRUE(statsWithSeconds(100).save(CACHE_PATH));
  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));
  EXPECT_TRUE(statsWithSeconds(200).save(CACHE_PATH));
  EXPECT_TRUE(Storage.exists(backupPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 200U);
  EXPECT_TRUE(statsWithSeconds(300).save(CACHE_PATH));  // back to the primary slot
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 300U);
  EXPECT_EQ(fileSize(statsPath), 81U);  // 73 B payload + seq + CRC
  EXPECT_FALSE(Storage.exists((statsPath + ".tmp").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, TornNewestSlotFallsBackToPreviousSave) {
  statsWithSeconds(100).save(CACHE_PATH);
  statsWithSeconds(200).save(CACHE_PATH);
  corrupt(backupPath);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 100U);
  // The next save overwrites the damaged slot, not the good one.
  statsWithSeconds(250).save(CACHE_PATH);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 250U);
  corrupt(backupPath);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 100U);
}

TEST_F(BookReadingStatsAtomicTest, LegacyFileLoadsAndIsReplacedFirst) {
  // Older firmware wrote a bare 73-byte v5 payload.
  HalFile legacy;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", statsPath, legacy));
  uint8_t data[73] = {5};
  data[3] = 42;  // totalReadingSeconds, LE
  legacy.write(data, sizeof(data));
  legacy.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 42U);

  statsWithSeconds(43).save(CACHE_PATH);
  EXPECT_EQ(fileSize(statsPath), 81U);
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 43U);
}

TEST_F(BookReadingStatsAtomicTest, IdenticalSaveSkipsWrite) {
  statsWithSeconds(123).save(CACHE_PATH);
  statsWithSeconds(123).save(CACHE_PATH);
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));  // a write would have gone to the second slot
  statsWithSeconds(124).save(CACHE_PATH);
  EXPECT_TRUE(Storage.exists(backupPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 124U);
}

TEST_F(BookReadingStatsAtomicTest, MigratesPreviousVersionFile) {
  HalFile v4;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", previousStatsPath, v4));
  uint8_t data[69] = {4};
  data[3] = 77;
  v4.write(data, sizeof(data));
  v4.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 77U);
}

TEST_F(BookReadingStatsAtomicTest, RemoveClearsBothSlots) {
  statsWithSeconds(123).save(CACHE_PATH);
  statsWithSeconds(456).save(CACHE_PATH);
  EXPECT_TRUE(BookReadingStats::remove(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 0U);
}

TEST_F(BookReadingStatsAtomicTest, FailedFallbackRemovalKeepsCanonicalStats) {
  HalFile v4;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", previousStatsPath, v4));
  uint8_t data[69] = {4};
  v4.write(data, sizeof(data));
  v4.close();
  statsWithSeconds(456).save(CACHE_PATH);
  Storage.failNextRemove(previousStatsPath);

  EXPECT_FALSE(BookReadingStats::remove(CACHE_PATH));

  EXPECT_TRUE(Storage.exists(statsPath.c_str()));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 456U);
}
