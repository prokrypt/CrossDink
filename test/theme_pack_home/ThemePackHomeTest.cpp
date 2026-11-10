#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "components/ThemePackHome.h"

namespace TPH = ThemePackHome;

namespace {

bool parse(const char* json, TPH::Layout& out) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, json));
  return TPH::parse(doc.as<JsonObjectConst>(), "home.json", out);
}

}  // namespace

TEST(ThemePackHome, ParsesThePlanExample) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [
      {"type": "header"},
      {"type": "covers", "style": "row", "count": 3, "height": "45%"},
      {"type": "stats", "items": ["streak", "today", "book_progress"]},
      {"type": "menu", "style": "grid", "items": ["continue", "library", "files", "opds", "folder:/Comics", "settings"]}
    ]})",
                    l));
  ASSERT_EQ(l.blockCount, 4);
  EXPECT_EQ(l.blocks[0].type, TPH::BlockType::Header);
  EXPECT_EQ(l.blocks[1].type, TPH::BlockType::Covers);
  EXPECT_EQ(l.blocks[1].style, TPH::Style::Row);
  EXPECT_EQ(l.blocks[1].count, 3);
  EXPECT_EQ(l.blocks[1].columns, 3);
  EXPECT_EQ(l.blocks[1].height, 45);
  EXPECT_EQ(l.blocks[2].count, 3);
  EXPECT_EQ(l.blocks[2].stats[2], TPH::Stat::BookProgress);
  EXPECT_EQ(l.blocks[3].style, TPH::Style::Grid);
  EXPECT_EQ(l.blocks[3].columns, 2);
  EXPECT_EQ(l.blocks[3].first, 0);
  ASSERT_EQ(l.blocks[3].count, 6);
  EXPECT_EQ(l.entries[4].action, TPH::Action::Folder);
  EXPECT_STREQ(l.entries[4].path, "/Comics");
  EXPECT_EQ(l.entries[5].action, TPH::Action::Settings);
  EXPECT_EQ(l.bookCount, 3);
}

TEST(ThemePackHome, SkipsUnknownBlocksEntriesAndKeys) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [
      {"type": "weather"}, 7,
      {"type": "menu", "colour": "red", "items": ["library", "page:comics", "nope", "book:relative.epub",
                                                  "folder:/../x", "goodies", "settings"]}
    ], "extra": 1})",
                    l));
  ASSERT_EQ(l.blockCount, 1);
  ASSERT_EQ(l.blocks[0].count, 2);  // library, settings (goodies: debug builds only)
  EXPECT_EQ(l.entries[0].action, TPH::Action::Library);
  EXPECT_EQ(l.entries[1].action, TPH::Action::Settings);
}

TEST(ThemePackHome, AppendsSettingsToTheLastMenu) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [{"type": "menu", "items": ["files"]}, {"type": "clock"},
                                    {"type": "menu", "items": ["library"]}]})",
                    l));
  ASSERT_EQ(l.blockCount, 3);
  EXPECT_EQ(l.blocks[2].first, 1);
  ASSERT_EQ(l.blocks[2].count, 2);
  EXPECT_EQ(l.entries[2].action, TPH::Action::Settings);
}

TEST(ThemePackHome, AddsASettingsMenuWhenThereIsNone) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [{"type": "covers", "style": "single"}]})", l));
  ASSERT_EQ(l.blockCount, 2);
  EXPECT_EQ(l.blocks[1].type, TPH::BlockType::Menu);
  EXPECT_EQ(l.blocks[1].style, TPH::Style::List);
  ASSERT_EQ(l.blocks[1].count, 1);
  EXPECT_EQ(l.entries[l.blocks[1].first].action, TPH::Action::Settings);
}

TEST(ThemePackHome, SettingsStaysReachableWhenBlocksAreFull) {
  std::string json = R"({"blocks": [)";
  for (int i = 0; i < 20; ++i) json += R"({"type": "spacer"},)";
  json += R"({"type": "menu", "items": ["files"]}]})";
  TPH::Layout l;
  ASSERT_TRUE(parse(json.c_str(), l));
  ASSERT_EQ(l.blockCount, TPH::kMaxBlocks);  // the menu past the limit is dropped; Settings gets its own
  EXPECT_EQ(l.blocks[l.blockCount - 1].type, TPH::BlockType::Menu);
  ASSERT_EQ(l.entryCount, 1);
  EXPECT_EQ(l.entries[0].action, TPH::Action::Settings);
}

TEST(ThemePackHome, SettingsStaysReachableWhenEntriesAreFull) {
  std::string json = R"({"blocks": [{"type": "menu", "items": [)";
  for (int i = 0; i < 30; ++i) json += R"("files",)";
  json += R"("files"]}]})";
  TPH::Layout l;
  ASSERT_TRUE(parse(json.c_str(), l));
  ASSERT_EQ(l.entryCount, TPH::kMaxEntries);
  EXPECT_EQ(l.blocks[0].count, TPH::kMaxEntries);
  EXPECT_EQ(l.entries[TPH::kMaxEntries - 1].action, TPH::Action::Settings);
}

TEST(ThemePackHome, CapsBooksAcrossCoversBlocks) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [{"type": "covers", "style": "single", "count": 5},
                                    {"type": "covers", "style": "grid", "count": 9, "columns": 9},
                                    {"type": "covers"}]})",
                    l));
  ASSERT_EQ(l.blockCount, 3);  // + the Settings menu; the third covers block had no books left
  EXPECT_EQ(l.blocks[0].count, 1);
  EXPECT_EQ(l.blocks[1].count, 6);
  EXPECT_EQ(l.blocks[1].columns, TPH::kMaxColumns);
  EXPECT_EQ(l.bookCount, TPH::kMaxBooks);
}

TEST(ThemePackHome, ParsesAndScalesHeights) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [{"type": "spacer", "height": "80%"}, {"type": "spacer", "height": "80%"},
                                    {"type": "spacer", "height": "flex"}, {"type": "spacer", "height": 40},
                                    {"type": "spacer", "height": "x%"}]})",
                    l));
  EXPECT_EQ(l.blocks[0].height, 50);
  EXPECT_EQ(l.blocks[1].height, 50);
  EXPECT_EQ(l.blocks[2].height, TPH::kFlex);
  EXPECT_EQ(l.blocks[3].height, 0);
  EXPECT_EQ(l.blocks[4].height, 0);
}

TEST(ThemePackHome, KeepsTextAndImageFiles) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [{"type": "text", "text": "Hello"}, {"type": "text", "text": ""},
                                    {"type": "image", "file": "art/a.bmp"}, {"type": "image", "file": "/abs.bmp"},
                                    {"type": "image", "file": "../up.bmp"}]})",
                    l));
  ASSERT_EQ(l.blockCount, 3);
  EXPECT_STREQ(l.texts[l.blocks[0].first], "Hello");
  EXPECT_EQ(l.imageCount, 1);
  EXPECT_STREQ(l.images[l.blocks[1].first], "art/a.bmp");
}

TEST(ThemePackHome, RejectsLayoutsWithNothingUsable) {
  TPH::Layout l;
  EXPECT_FALSE(parse(R"({"blocks": []})", l));
  EXPECT_FALSE(parse(R"({"blocks": [{"type": "nope"}, {"type": "stats", "items": ["x"]}]})", l));
  EXPECT_FALSE(parse(R"({"layout": []})", l));
  EXPECT_FALSE(parse(R"({"blocks": {"type": "header"}})", l));
}
