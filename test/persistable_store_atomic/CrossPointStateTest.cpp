#include <CrossPointState.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

namespace {
constexpr char STATE_PATH[] = "/.crossdink/state.json";
}

class CrossPointStateTest : public testing::Test {
 protected:
  void SetUp() override {
    // APP_STATE outlives each test, as on device; start each from a card that
    // holds the defaults so the remembered on-disk snapshot matches it.
    Storage.reset();
    Storage.put(STATE_PATH, "{}");
    ASSERT_TRUE(APP_STATE.loadFromFile());
  }
};

TEST_F(CrossPointStateTest, SkipsRepeatedSuccessfulSnapshot) {
  APP_STATE.openEpubPath = "/book.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 1U);
}

TEST_F(CrossPointStateTest, SkipsSaveThatMatchesLoadedFile) {
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 0U);
}

// The reader crash guard lives in RTC memory, so opening a book costs no write.
TEST_F(CrossPointStateTest, ReaderLoadGuardCostsNoWrite) {
  APP_STATE.setReaderActivityLoadCount(1);
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(APP_STATE.readerActivityLoadCount(), 1);
  APP_STATE.setReaderActivityLoadCount(0);
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(APP_STATE.readerActivityLoadCount(), 0);
  EXPECT_EQ(Storage.writeAttempts, 0U);
}

TEST_F(CrossPointStateTest, PersistsWallpaperChanges) {
  APP_STATE.pushRecentSleep(9);
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 1U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_TRUE(APP_STATE.isRecentSleep(9, 1));
}

TEST_F(CrossPointStateTest, RetriesFailedFirstSave) {
  APP_STATE.openEpubPath = "/book.epub";
  Storage.failNextWrite();
  EXPECT_FALSE(APP_STATE.saveToFile());
  EXPECT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 2U);
}

TEST_F(CrossPointStateTest, FailedRewriteInvalidatesPreviousSnapshot) {
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  APP_STATE.openEpubPath = "/new.epub";
  Storage.failNextWrite();
  EXPECT_FALSE(APP_STATE.saveToFile());
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 3U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/old.epub");
}

TEST_F(CrossPointStateTest, ReloadInvalidatesPreviousSnapshot) {
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  Storage.put(STATE_PATH, R"({"openEpubPath":"/new.epub"})");
  ASSERT_TRUE(APP_STATE.loadFromFile());
  APP_STATE.openEpubPath = "/old.epub";
  ASSERT_TRUE(APP_STATE.saveToFile());
  EXPECT_EQ(Storage.writeAttempts, 2U);
  ASSERT_TRUE(APP_STATE.loadFromFile());
  EXPECT_EQ(APP_STATE.openEpubPath, "/old.epub");
}
