#include <HalClock.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "DailyReadingStats.h"
#include "ReadingSessionJournal.h"
#include "sync/SyncFiles.h"
#include "util/StatsStorage.h"

namespace DailyReadingStats {
void resetForTest();
}

namespace {
constexpr char ME[] = "crossink-000000000000";  // the esp_mac stub's all-zero MAC
constexpr char OTHER[] = "crossink-0123456789ab";
const std::string LEGACY_DAILY = DailyReadingStats::DIRECTORY;
const std::string LOCAL_DAILY = LEGACY_DAILY + "_" + ME;
const std::string LEGACY_OUTBOX = "/.crossdink/reading_sessions";
const std::string LOCAL_OUTBOX = LEGACY_OUTBOX + "_" + ME;
constexpr char ROOT_CLAIM[] = "/.crossdink/stats_origin.bin";

void writeClaim(const char* path, const char* id) {
  SyncFiles::Writer writer(path);
  char stored[32]{};
  snprintf(stored, sizeof(stored), "%s", id);
  ASSERT_TRUE(writer.open() && writer.pod(uint8_t{1}) && writer.write(stored, sizeof(stored)) && writer.commit());
}

uint32_t october(const uint8_t day) { return readingStatsDayIndex({2026, 10, day}); }

uint32_t secondsOn(const uint8_t day) {
  DailyReadingStats::Counter counter;
  EXPECT_TRUE(DailyReadingStats::read(october(day), counter));
  return counter.seconds;
}

// What an older build left: its daily folder, holding `seconds` on October 1.
void olderBuildDaily(const uint32_t seconds) {
  ASSERT_TRUE(DailyReadingStats::write(october(1), {seconds, 0}));
  ASSERT_TRUE(Storage.rename(LOCAL_DAILY.c_str(), LEGACY_DAILY.c_str()));
  Storage.remove(ROOT_CLAIM);
  DailyReadingStats::resetForTest();
  StatsStorage::forget();
}

ReadingSessionJournal::Record session(const unsigned index) {
  ReadingSessionJournal::Record record;
  snprintf(record.id, sizeof(record.id), "%032x", index);
  strcpy(record.document, "0123456789abcdef0123456789abcdef");
  record.startedAt = 946684800ULL + index * 120;
  record.endedAt = record.startedAt + 60;
  record.seconds = 60;
  return record;
}

bool collect(void* rows, const ReadingSessionJournal::Record& record) {
  static_cast<std::vector<std::string>*>(rows)->push_back(record.id);
  return true;
}
}  // namespace

class StatsFolders : public testing::Test {
 protected:
  void SetUp() override {
    Storage.reset();
    DailyReadingStats::resetForTest();
    StatsStorage::forget();
    halClock = {};
    halClock.day = 3;
  }
};

TEST_F(StatsFolders, CountersLiveInThisReadersFolder) {
  ASSERT_TRUE(DailyReadingStats::write(october(1), {60, 0}));
  // The longest month path still fits the 64-byte path buffers.
  EXPECT_TRUE(Storage.exists((LOCAL_DAILY + "/2026-10.a.bin").c_str()));
  EXPECT_FALSE(Storage.exists(LEGACY_DAILY.c_str()));
}

TEST_F(StatsFolders, OlderBuildsFolderIsReadThenMovedByTheFirstWrite) {
  olderBuildDaily(60);
  const auto before = Storage.files.size();
  EXPECT_EQ(secondsOn(1), 60u);  // reads use it in place
  EXPECT_EQ(Storage.files.size(), before);
  EXPECT_TRUE(Storage.exists(LEGACY_DAILY.c_str()));
  ASSERT_TRUE(DailyReadingStats::write(october(2), {30, 0}));
  EXPECT_FALSE(Storage.exists(LEGACY_DAILY.c_str()));
  EXPECT_EQ(secondsOn(1), 60u);
  EXPECT_EQ(secondsOn(2), 30u);
}

TEST_F(StatsFolders, AnotherReadersFolderStaysAndIsNotRead) {
  olderBuildDaily(60);
  writeClaim(ROOT_CLAIM, OTHER);
  EXPECT_EQ(secondsOn(1), 0u);
  ASSERT_TRUE(DailyReadingStats::write(october(1), {5, 0}));
  EXPECT_TRUE(Storage.exists((LEGACY_DAILY + "/2026-10.a.bin").c_str()));
  EXPECT_EQ(secondsOn(1), 5u);
}

TEST_F(StatsFolders, UnreadableOriginBlocksWrites) {
  olderBuildDaily(60);
  ASSERT_TRUE(Storage.writeFile(ROOT_CLAIM, std::string(37, '\x01')));
  EXPECT_FALSE(DailyReadingStats::write(october(1), {70, 0}));
  EXPECT_TRUE(ReadingSessionJournal::save(session(1)));  // no older outbox: nothing to protect
  EXPECT_TRUE(Storage.exists(LEGACY_DAILY.c_str()));
  EXPECT_FALSE(Storage.exists(LOCAL_DAILY.c_str()));
}

TEST_F(StatsFolders, RollbackLeftoversMoveFileByFileKeepingThisReadersCopy) {
  ASSERT_TRUE(DailyReadingStats::write(october(1), {60, 0}));  // this reader's October
  // An older build after a rollback: a clashing October and a new November.
  ASSERT_TRUE(Storage.mkdir(LEGACY_DAILY.c_str()));
  for (const char* name : {"/2026-10.a.bin", "/2026-11.a.bin"}) {
    ASSERT_TRUE(Storage.writeFile((LEGACY_DAILY + name).c_str(), "older"));
  }
  DailyReadingStats::resetForTest();
  ASSERT_TRUE(DailyReadingStats::write(october(2), {30, 0}));
  EXPECT_TRUE(Storage.exists((LOCAL_DAILY + "/2026-11.a.bin").c_str()));
  EXPECT_TRUE(Storage.exists((LEGACY_DAILY + "/2026-10.a.bin").c_str()));  // kept, not overwritten
  EXPECT_FALSE(Storage.exists((LEGACY_DAILY + "/2026-11.a.bin").c_str()));
  EXPECT_EQ(secondsOn(1), 60u);
}

TEST_F(StatsFolders, AnotherReadersTwinIsHiddenBeforeTheMove) {
  olderBuildDaily(60);
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/daily_reading"));
  writeClaim("/.crosspoint/stats_origin.bin", OTHER);
  ASSERT_TRUE(DailyReadingStats::write(october(2), {30, 0}));
  EXPECT_TRUE(Storage.hidden.contains(LEGACY_DAILY));
  EXPECT_EQ(secondsOn(1), 60u);
}

TEST_F(StatsFolders, OwnTwinIsMergedByTheMove) {
  olderBuildDaily(60);
  ASSERT_TRUE(Storage.mkdir("/.crosspoint/daily_reading"));  // no /.crosspoint record: this reader's
  ASSERT_TRUE(DailyReadingStats::write(october(2), {30, 0}));
  EXPECT_TRUE(Storage.hidden.empty());
}

TEST_F(StatsFolders, TwinOnlyFolderFollowsTheTwinRecord) {
  olderBuildDaily(60);
  Storage.twinOnly.insert(LEGACY_DAILY);
  writeClaim("/.crosspoint/stats_origin.bin", OTHER);
  writeClaim(ROOT_CLAIM, ME);
  EXPECT_EQ(secondsOn(1), 0u);
  ASSERT_TRUE(DailyReadingStats::write(october(2), {30, 0}));
  EXPECT_TRUE(Storage.exists(LEGACY_DAILY.c_str()));  // CrossInk's, left alone
}

TEST_F(StatsFolders, OwnOlderOutboxMovesAndUploads) {
  ASSERT_TRUE(ReadingSessionJournal::save(session(1)));
  ASSERT_TRUE(Storage.rename(LOCAL_OUTBOX.c_str(), LEGACY_OUTBOX.c_str()));
  std::vector<std::string> rows;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collect, &rows));
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_FALSE(Storage.exists(LEGACY_OUTBOX.c_str()));
}

TEST_F(StatsFolders, AnotherReadersPendingSessionsAreKeptNotUploaded) {
  ASSERT_TRUE(ReadingSessionJournal::save(session(1)));
  ASSERT_TRUE(Storage.rename(LOCAL_OUTBOX.c_str(), LEGACY_OUTBOX.c_str()));
  Storage.remove(ROOT_CLAIM);
  writeClaim(ROOT_CLAIM, OTHER);
  StatsStorage::forget();
  std::vector<std::string> rows;
  ASSERT_TRUE(ReadingSessionJournal::uploadPending(collect, &rows));
  EXPECT_TRUE(rows.empty());
  EXPECT_TRUE(Storage.exists((LEGACY_OUTBOX + "/" + session(1).id + ".bin").c_str()));
}
