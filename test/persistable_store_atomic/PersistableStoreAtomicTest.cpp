#include <HalStorage.h>
#include <PersistableStore.h>
#include <gtest/gtest.h>

namespace {
constexpr char STORE_PATH[] = "/.crossdink/recent.json";

JsonDocument documentWithPath(const char* path) {
  JsonDocument doc;
  doc["books"][0]["path"] = path;
  return doc;
}

const char* firstPath(const JsonDocument& doc) { return doc["books"][0]["path"] | ""; }
}  // namespace

class PersistableStoreAtomicTest : public testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST_F(PersistableStoreAtomicTest, LoadsBackupWhenTornWriteLeftStoreUnparsable) {
  ASSERT_TRUE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/new.epub")));
  Storage.put(STORE_PATH, R"({"books":[{"pa)");

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/new.epub");
}

TEST_F(PersistableStoreAtomicTest, FailsWhenStoreAndBackupAreUnparsable) {
  Storage.put(STORE_PATH, "{");
  Storage.put(std::string(STORE_PATH) + ".bak", "{");

  JsonDocument loaded;
  EXPECT_FALSE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
}

TEST_F(PersistableStoreAtomicTest, RecoversBackupAfterInterruptedReplacement) {
  Storage.put(std::string(STORE_PATH) + ".bak", R"({"books":[{"path":"/old.epub"}]})");

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/old.epub");
  EXPECT_TRUE(Storage.exists(STORE_PATH));
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".bak").c_str()));
}

TEST_F(PersistableStoreAtomicTest, RewritesBothCopiesInPlaceWithoutTemporaryFile) {
  ASSERT_TRUE(PersistableStoreBase::writeDocToFile(STORE_PATH, documentWithPath("/old.epub")));
  ASSERT_TRUE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/new.epub")));

  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/new.epub");
  EXPECT_EQ(Storage.readFile((std::string(STORE_PATH) + ".bak").c_str()), Storage.readFile(STORE_PATH));
  EXPECT_FALSE(Storage.exists((std::string(STORE_PATH) + ".tmp").c_str()));
}

TEST_F(PersistableStoreAtomicTest, ShorterSavePadsWithSpacesAndStillParses) {
  ASSERT_TRUE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/a-long-book-name.epub")));
  ASSERT_TRUE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/b.epub")));

  const String onCard = Storage.readFile(STORE_PATH);
  EXPECT_EQ(onCard.back(), ' ');
  JsonDocument loaded;
  ASSERT_TRUE(PersistableStoreBase::readDocFromFile(STORE_PATH, loaded));
  EXPECT_STREQ(firstPath(loaded), "/b.epub");
  // The same document again is recognised through the padding and not rewritten.
  Storage.put(std::string(STORE_PATH) + ".bak", "sentinel");
  ASSERT_TRUE(PersistableStoreBase::writeDocToFileAtomically(STORE_PATH, documentWithPath("/b.epub")));
  EXPECT_EQ(Storage.readFile((std::string(STORE_PATH) + ".bak").c_str()), "sentinel");
}
