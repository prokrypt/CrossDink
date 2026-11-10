#include <BookReadingStats.h>
#include <HalStorage.h>
#include <ReadingPace.h>
#include <gtest/gtest.h>

#include <string>

#include "TwoSlotFile.h"
#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

namespace {
constexpr char CACHE_PATH[] = "/cache/book";
constexpr char ME[] = "crossink-000000000000";  // the esp_mac stub's all-zero MAC
constexpr char OTHER[] = "crossink-0123456789ab";
const std::string statsPath = std::string(CACHE_PATH) + "/stats_v6_" + ME + ".bin";
const std::string unqualifiedV6Path = std::string(CACHE_PATH) + "/stats_v6.bin";
const std::string folderClaim = std::string(CACHE_PATH) + "/stats_origin.bin";
constexpr char ROOT_CLAIM[] = "/.crossdink/stats_origin.bin";
const std::string backupPath = statsPath + ".bak";
const std::string previousStatsPath = std::string(CACHE_PATH) + "/stats_v5.bin";
const std::string v4StatsPath = std::string(CACHE_PATH) + "/stats_v4.bin";

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

void writeClaim(const std::string& path, const char* id) {
  SyncFiles::Writer writer(path.c_str());
  char stored[32]{};
  snprintf(stored, sizeof(stored), "%s", id);
  ASSERT_TRUE(writer.open() && writer.pod(uint8_t{1}) && writer.write(stored, sizeof(stored)) && writer.commit());
}

std::string claimOwner(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("TEST", path, file)) return {};
  char data[37]{};
  file.read(data, sizeof(data));
  file.close();
  return std::string(data + 1);
}

// A bare v5 payload as older firmware wrote it.
void writeV5(const std::string& path, const uint8_t seconds) {
  HalFile file;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", path, file));
  uint8_t data[73] = {5};
  data[3] = seconds;
  file.write(data, sizeof(data));
  file.close();
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
  void SetUp() override {
    Storage.reset();
    StatsStorage::forget();
  }
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
  EXPECT_EQ(fileSize(statsPath), 83U);  // 75 B payload + seq + CRC
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
  ASSERT_TRUE(Storage.openFileForWrite("TEST", previousStatsPath, legacy));
  uint8_t data[73] = {5};
  data[3] = 42;  // totalReadingSeconds, LE
  legacy.write(data, sizeof(data));
  legacy.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 42U);

  statsWithSeconds(43).save(CACHE_PATH);
  EXPECT_EQ(fileSize(statsPath), 83U);
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
  ASSERT_TRUE(Storage.openFileForWrite("TEST", v4StatsPath, v4));
  uint8_t data[69] = {4};
  data[3] = 77;
  v4.write(data, sizeof(data));
  v4.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 77U);
}

TEST_F(BookReadingStatsAtomicTest, MigratesVersionFiveFromItsSecondSlot) {
  // The previous firmware saved v5 in two slots; the newest save may sit in the backup.
  HalFile slot;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", previousStatsPath + ".bak", slot));
  uint8_t data[73] = {5};
  data[3] = 200;
  data[12] = 30;  // avgSecondsPerForwardPage
  data[14] = 9;   // paceSampleCount
  slot.write(data, sizeof(data));
  slot.close();
  const auto loaded = BookReadingStats::load(CACHE_PATH);
  EXPECT_EQ(loaded.totalReadingSeconds, 200U);
  EXPECT_EQ(loaded.averageForwardPageMillis(), 30000U);
  EXPECT_EQ(loaded.paceFractionMillis, 0U);
  ASSERT_TRUE(loaded.save(CACHE_PATH));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).averageForwardPageMillis(), 30000U);
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

TEST_F(BookReadingStatsAtomicTest, RemoveClearsEveryOlderVersion) {
  for (const std::string& path :
       {previousStatsPath, previousStatsPath + ".bak", v4StatsPath, std::string(CACHE_PATH) + "/stats.bin"}) {
    HalFile f;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", path, f));
    uint8_t data[73] = {5};
    f.write(data, sizeof(data));
    f.close();
  }
  statsWithSeconds(456).save(CACHE_PATH);
  EXPECT_TRUE(BookReadingStats::remove(CACHE_PATH));
  for (const std::string& path : {statsPath, backupPath, previousStatsPath, previousStatsPath + ".bak", v4StatsPath,
                                  std::string(CACHE_PATH) + "/stats.bin"}) {
    EXPECT_FALSE(Storage.exists(path.c_str())) << path;
  }
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 0U);
}

TEST_F(BookReadingStatsAtomicTest, FractionSurvivesSaveAndReopen) {
  BookReadingStats stats;
  stats.recordForwardPageRead(29);
  stats.recordForwardPageRead(30);
  EXPECT_EQ(stats.averageForwardPageMillis(), 29500U);
  ASSERT_TRUE(stats.save(CACHE_PATH));
  const auto loaded = BookReadingStats::load(CACHE_PATH);
  EXPECT_EQ(loaded.averageForwardPageMillis(), 29500U);
  EXPECT_EQ(loaded.paceSampleCount, 2U);
}

TEST_F(BookReadingStatsAtomicTest, AlternatingSamplesDoNotRoundDownEachUpdate) {
  BookReadingStats stats;
  stats.setAverageForwardPageMillis(30000);
  stats.paceSampleCount = 100;
  for (int i = 0; i < 20; ++i) stats.recordForwardPageRead(i % 2 == 0 ? 29 : 31);
  EXPECT_NEAR(stats.averageForwardPageMillis(), 30000U, 5);
}

TEST_F(BookReadingStatsAtomicTest, EstablishedPaceCanMoveInBothDirections) {
  for (const uint32_t sample : {20U, 40U}) {
    BookReadingStats stats;
    stats.setAverageForwardPageMillis(30000);
    stats.paceSampleCount = 100;
    for (int i = 0; i < 20; ++i) stats.recordForwardPageRead(sample);
    const double expectedMillis = (100.0 * 30000 + 20.0 * sample * 1000) / 120;
    EXPECT_NEAR(stats.averageForwardPageMillis(), expectedMillis, 10);
  }
}

TEST_F(BookReadingStatsAtomicTest, SaturatedCountAndMaximumSampleDoNotOverflow) {
  BookReadingStats stats;
  stats.setAverageForwardPageMillis(60000000);
  stats.paceSampleCount = 1000;
  stats.recordForwardPageRead(UINT32_MAX);
  EXPECT_EQ(stats.paceSampleCount, 1000U);
  EXPECT_GT(stats.averageForwardPageMillis(), 60000000U);
  EXPECT_LE(stats.averageForwardPageMillis(), 65535000U);
  stats.setAverageForwardPageMillis(UINT32_MAX);
  EXPECT_EQ(stats.averageForwardPageMillis(), 65535000U);
  EXPECT_EQ(stats.paceFractionMillis, 0U);
  stats.recordForwardPageRead(0);
  EXPECT_EQ(stats.averageForwardPageMillis(), 65535000U);
}

TEST_F(BookReadingStatsAtomicTest, RejectsInvalidFraction) {
  BookReadingStats stats;
  stats.paceFractionMillis = 1000;
  ASSERT_TRUE(stats.save(CACHE_PATH));
  EXPECT_FALSE(BookReadingStats::loadForUpload(CACHE_PATH, stats));
}

TEST_F(BookReadingStatsAtomicTest, InvalidFractionSlotFallsBackToValidSlot) {
  ASSERT_TRUE(statsWithSeconds(123).save(CACHE_PATH));
  auto invalid = statsWithSeconds(456);
  invalid.paceFractionMillis = 1000;
  ASSERT_TRUE(invalid.save(CACHE_PATH));  // newer slot, rejected on read
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 123U);
}

TEST_F(BookReadingStatsAtomicTest, ShortSessionReopensAtTheSameBlendedPace) {
  BookReadingStats stats;
  stats.setAverageForwardPageMillis(30000);
  stats.paceSampleCount = 100;
  const auto prior = stats.averageForwardPageMillis();
  const auto priorCount = stats.paceSampleCount;
  for (uint16_t count = 1; count <= 4; ++count) {
    stats.recordForwardPageRead(40);
    stats.setAverageForwardPageMillis(ReadingPace::estimateMillis(prior, priorCount, 40 * count, count));
  }
  ASSERT_TRUE(stats.save(CACHE_PATH));
  stats = BookReadingStats::load(CACHE_PATH);
  EXPECT_EQ(stats.averageForwardPageMillis(), 34000U);
  EXPECT_EQ(ReadingPace::estimateMillis(stats.averageForwardPageMillis(), stats.paceSampleCount, 0, 0), 34000U);
  stats.setAverageForwardPageMillis(0);
  stats.paceSampleCount = 0;
  EXPECT_EQ(stats.paceFractionMillis, 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(stats.averageForwardPageMillis(), stats.paceSampleCount, 0, 0), 0U);
}

TEST(ReadingPace, NewReaderKeepsThreeSampleWarmup) {
  EXPECT_EQ(ReadingPace::estimateMillis(0, 0, 60, 2), 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(0, 0, 90, 3), 30000U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 2, 0, 0), 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 3, 0, 0), 30000U);
}

TEST(ReadingPace, FasterAndSlowerSessionsHaveNoTenSampleCliff) {
  for (const uint32_t sample : {20U, 40U}) {
    uint32_t previous = 30000;
    for (uint16_t count = 1; count <= 12; ++count) {
      const uint32_t current = ReadingPace::estimateMillis(30000, 100, sample * count, count);
      EXPECT_LE(current > previous ? current - previous : previous - current, 1000U);
      if (count >= 10) EXPECT_EQ(current, sample * 1000);
      previous = current;
    }
  }
}

TEST(ReadingPace, RetainsFractionalSessionMeanAndHandlesLargeTotals) {
  EXPECT_EQ(ReadingPace::sessionAverageMillis(295, 10), 29500U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 100, 295, 10), 29500U);
  EXPECT_EQ(ReadingPace::sessionAverageMillis(UINT32_MAX, UINT16_MAX), 65535000U);
  EXPECT_EQ(ReadingPace::sessionAverageMillis(UINT32_MAX, 0), 0U);
}

TEST(ReadingPace, LearningSamplesCarryAcrossShortSessions) {
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 1, 30, 1), 0U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 2, 30, 1), 30000U);
  EXPECT_EQ(ReadingPace::estimateMillis(30000, 1, 60, 2), 30000U);
  // A two-sample prior has less influence than a well-established prior.
  EXPECT_GT(ReadingPace::estimateMillis(30000, 2, 40, 1), ReadingPace::estimateMillis(30000, 100, 40, 1));
}

// --- Per-reader files (util/StatsStorage.h) ---

TEST_F(BookReadingStatsAtomicTest, OlderBuildsV6SlotsLoadAndStayForOlderFirmware) {
  uint8_t payload[75] = {6};
  payload[3] = 90;
  ASSERT_TRUE(two_slot::write("TEST", unqualifiedV6Path.c_str(), (unqualifiedV6Path + ".bak").c_str(), payload, 75));
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 90U);
  ASSERT_TRUE(statsWithSeconds(91).save(CACHE_PATH));
  EXPECT_EQ(fileSize(statsPath), 83U);
  EXPECT_EQ(fileSize(unqualifiedV6Path), 83U);  // a rollback still finds its stats
  EXPECT_EQ(claimOwner(ROOT_CLAIM), ME);
  EXPECT_EQ(claimOwner(folderClaim), ME);  // travels with a copy of only this folder
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 91U);
}

TEST_F(BookReadingStatsAtomicTest, LoadNeverWrites) {
  writeV5(previousStatsPath, 42);
  const auto before = Storage.paths();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 42U);
  EXPECT_EQ(Storage.paths(), before);
}

TEST_F(BookReadingStatsAtomicTest, AnotherReadersFolderIsLeftAlone) {
  writeV5(previousStatsPath, 42);
  writeClaim(folderClaim, OTHER);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 0U);
  ASSERT_TRUE(statsWithSeconds(5).save(CACHE_PATH));  // this reader starts its own copy
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 5U);
  EXPECT_TRUE(BookReadingStats::remove(CACHE_PATH));
  EXPECT_FALSE(Storage.exists(statsPath.c_str()));
  EXPECT_TRUE(Storage.exists(previousStatsPath.c_str()));  // still the other reader's
  EXPECT_EQ(claimOwner(folderClaim), OTHER);
}

TEST_F(BookReadingStatsAtomicTest, RootClaimNamesTheOwnerOfUnclaimedFolders) {
  writeV5(previousStatsPath, 42);
  writeClaim(ROOT_CLAIM, OTHER);
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 0U);
  ASSERT_TRUE(statsWithSeconds(5).save(CACHE_PATH));
  EXPECT_EQ(claimOwner(folderClaim), OTHER);  // recorded before this reader's first save
  EXPECT_EQ(claimOwner(ROOT_CLAIM), OTHER);
}

TEST_F(BookReadingStatsAtomicTest, UnreadableClaimRefusesSaveAndReset) {
  writeV5(previousStatsPath, 42);
  writeClaim(folderClaim, ME);
  corrupt(folderClaim);  // complete size, wrong CRC
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 0U);
  EXPECT_FALSE(statsWithSeconds(5).save(CACHE_PATH));  // would hide the 42 s it could not read
  EXPECT_FALSE(Storage.exists(statsPath.c_str()));
  EXPECT_FALSE(BookReadingStats::remove(CACHE_PATH));
  EXPECT_TRUE(Storage.exists(previousStatsPath.c_str()));
}

TEST_F(BookReadingStatsAtomicTest, TornFirstClaimIsRetried) {
  writeV5(previousStatsPath, 42);
  HalFile torn;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", folderClaim + ".tmp", torn));
  torn.write(reinterpret_cast<const uint8_t*>("\x01"
                                              "cross"),
             6);
  torn.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 42U);
  ASSERT_TRUE(statsWithSeconds(43).save(CACHE_PATH));
  EXPECT_EQ(claimOwner(folderClaim), ME);
  EXPECT_FALSE(Storage.exists((folderClaim + ".tmp").c_str()));
}

TEST_F(BookReadingStatsAtomicTest, NewerClaimFormatIsUnreadable) {
  writeV5(previousStatsPath, 42);
  HalFile claim;
  ASSERT_TRUE(Storage.openFileForWrite("TEST", folderClaim, claim));
  uint8_t data[37] = {2};
  claim.write(data, sizeof(data));
  claim.close();
  EXPECT_EQ(BookReadingStats::load(CACHE_PATH).totalReadingSeconds, 0U);
  EXPECT_FALSE(statsWithSeconds(5).save(CACHE_PATH));
}

TEST_F(BookReadingStatsAtomicTest, TwinFilesFollowOnlyTwinClaims) {
  // CrossInk on another reader recorded /.crosspoint; this reader recorded /.crossdink.
  writeClaim("/.crosspoint/stats_origin.bin", OTHER);
  writeClaim(ROOT_CLAIM, ME);
  const std::string own = "/.crossdink/xtc_1";
  const std::string twin = "/.crossdink/xtc_2";
  writeV5(own + "/stats_v5.bin", 11);
  writeV5(twin + "/stats_v5.bin", 22);
  Storage.markTwinOnly(twin + "/stats_v5.bin");
  EXPECT_EQ(BookReadingStats::load(own).totalReadingSeconds, 11U);
  EXPECT_EQ(BookReadingStats::load(twin).totalReadingSeconds, 0U);
  ASSERT_TRUE(statsWithSeconds(1).save(twin));
  EXPECT_FALSE(Storage.exists((twin + "/stats_origin.bin").c_str()));   // its owner lives under /.crosspoint
  EXPECT_FALSE(Storage.exists("/.crosspoint/xtc_2/stats_origin.bin"));  // never written

  // A twin folder's own record wins over the twin root's.
  writeClaim("/.crosspoint/xtc_3/stats_origin.bin", ME);
  writeV5("/.crossdink/xtc_3/stats_v5.bin", 33);
  Storage.markTwinOnly("/.crossdink/xtc_3/stats_v5.bin");
  EXPECT_EQ(BookReadingStats::load("/.crossdink/xtc_3").totalReadingSeconds, 33U);
}

TEST_F(BookReadingStatsAtomicTest, UnclaimedTwinFilesCountAsThisReaders) {
  writeV5("/.crossdink/xtc_1/stats_v5.bin", 22);
  Storage.markTwinOnly("/.crossdink/xtc_1/stats_v5.bin");
  EXPECT_EQ(BookReadingStats::load("/.crossdink/xtc_1").totalReadingSeconds, 22U);
}
