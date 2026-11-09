#include <gtest/gtest.h>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "util/OpdsDownloadFolder.h"

// The legacy global settings are outside this store's test; migration uses only
// their existing OPDS fields and save method.
void CrossPointSettings::toJson(JsonDocument&) const {}
uint8_t CrossPointSettings::defaultUiScale() { return 1; }
bool CrossPointSettings::saveToFile() const { return true; }

TEST(OpdsServerStoreTest, ExistingValuesAndUnknownDefaultsRemainCompatible) {
  EXPECT_EQ(static_cast<unsigned>(OpdsFilenameFormat::AUTHOR_TITLE), 0u);
  EXPECT_EQ(static_cast<unsigned>(OpdsFilenameFormat::TITLE_AUTHOR), 1u);
  EXPECT_EQ(opdsFilenameFormatFromJson(nullptr), OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(opdsFilenameFormatFromJson(""), OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(opdsFilenameFormatFromJson("future_format"), OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_STREQ(opdsFilenameFormatToJson(static_cast<OpdsFilenameFormat>(255)), "author_title");
}

TEST(OpdsServerStoreTest, SavesAndReloadsFilenameOptionsWithoutChangingCredentials) {
  Storage.reset();
  auto& store = OPDS_STORE;
  store.release();
  store.loadFromFile();
  const OpdsFilenameFormat formats[] = {OpdsFilenameFormat::AUTHOR_TITLE, OpdsFilenameFormat::TITLE_AUTHOR,
                                        OpdsFilenameFormat::TITLE_AUTHOR};
  for (size_t i = 0; i < 3; ++i) {
    OpdsServer server{"Library", "https://example.test/opds", "reader", "secret", formats[i], {}, i == 2};
    ASSERT_TRUE(store.addServer(server));
  }
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  ASSERT_EQ(store.getCount(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    ASSERT_NE(store.getServer(i), nullptr);
    EXPECT_EQ(store.getServer(i)->filenameFormat, formats[i]);
    EXPECT_EQ(store.getServer(i)->useServerFilename, i == 2);
    EXPECT_EQ(store.getServer(i)->username, "reader");
    EXPECT_EQ(store.getServer(i)->password, "secret");
  }
}

TEST(OpdsServerStoreTest, OldMissingOrUnknownFieldsLoadAsAuthorTitle) {
  Storage.reset();
  Storage.put(
      OpdsServerStore::getFilePath(),
      R"({"servers":[{"name":"old"},{"filenameFormat":"future_format"},{"filenameFormat":"title_author"},{"filenameFormat":"server_filename"}]})");
  auto& store = OPDS_STORE;
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  ASSERT_EQ(store.getCount(), 4u);
  EXPECT_EQ(store.getServer(0)->filenameFormat, OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(store.getServer(1)->filenameFormat, OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_EQ(store.getServer(2)->filenameFormat, OpdsFilenameFormat::TITLE_AUTHOR);
  EXPECT_EQ(store.getServer(3)->filenameFormat, OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_FALSE(store.getServer(0)->useServerFilename);
  EXPECT_FALSE(store.getServer(1)->useServerFilename);
  EXPECT_FALSE(store.getServer(2)->useServerFilename);
  EXPECT_TRUE(store.getServer(3)->useServerFilename);
  ASSERT_TRUE(store.saveToFile());
  store.release();
  ASSERT_TRUE(store.loadFromFile());
  EXPECT_TRUE(store.getServer(3)->useServerFilename);
}

TEST(OpdsServerStoreTest, FilenamePartialUpdatesPreserveBothOptionsAndMigrateLegacySelection) {
  JsonDocument doc;
  OpdsServer server;
  EXPECT_FALSE(server.useServerFilename);
  server.filenameFormat = OpdsFilenameFormat::TITLE_AUTHOR;
  server.useServerFilename = true;
  ASSERT_EQ(deserializeJson(doc, R"({"name":"Older client"})"), DeserializationError::Ok);
  ASSERT_TRUE(applyOpdsFilenameSettings(doc.as<JsonVariantConst>(), server));
  EXPECT_EQ(server.filenameFormat, OpdsFilenameFormat::TITLE_AUTHOR);
  EXPECT_TRUE(server.useServerFilename);
  doc["useServerFilename"] = false;
  ASSERT_TRUE(applyOpdsFilenameSettings(doc.as<JsonVariantConst>(), server));
  EXPECT_EQ(server.filenameFormat, OpdsFilenameFormat::TITLE_AUTHOR);
  EXPECT_FALSE(server.useServerFilename);
  ASSERT_EQ(deserializeJson(doc, R"({"filenameFormat":"server_filename"})"), DeserializationError::Ok);
  ASSERT_TRUE(applyOpdsFilenameSettings(doc.as<JsonVariantConst>(), server));
  EXPECT_EQ(server.filenameFormat, OpdsFilenameFormat::AUTHOR_TITLE);
  EXPECT_TRUE(server.useServerFilename);
  // An explicit toggle value wins over a legacy format in mixed-client updates.
  doc["useServerFilename"] = false;
  ASSERT_TRUE(applyOpdsFilenameSettings(doc.as<JsonVariantConst>(), server));
  EXPECT_FALSE(server.useServerFilename);
  for (const char* json : {R"({"useServerFilename":null})", R"({"useServerFilename":1})",
                           R"({"useServerFilename":"false"})", R"({"useServerFilename":[]})"}) {
    ASSERT_EQ(deserializeJson(doc, json), DeserializationError::Ok);
    EXPECT_FALSE(applyOpdsFilenameSettings(doc.as<JsonVariantConst>(), server)) << json;
    EXPECT_FALSE(server.useServerFilename);
  }
}
