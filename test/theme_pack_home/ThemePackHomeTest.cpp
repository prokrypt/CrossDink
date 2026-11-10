#include <gtest/gtest.h>

#include <cstring>
#include <fstream>
#include <string>

#include "Logging.h"
#include "components/ThemePackHome.h"

namespace TPH = ThemePackHome;

namespace {

bool parse(const char* json, TPH::Layout& out) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, json));
  return TPH::parse(doc.as<JsonObjectConst>(), "home.json", out);
}

bool parsePage(const char* json, TPH::Layout& out) {
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, json));
  return TPH::parsePage(doc.as<JsonObjectConst>(), "pages/Comics.json", "Comics", out);
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
      {"type": "menu", "colour": "red", "items": ["library", "page:", "page:../x", "nope",
                                                  "book:relative.epub", "folder:/../x", "goodies", "settings"]}
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

TEST(ThemePackHome, ParsesPageEntries) {
  TPH::Layout l;
  ASSERT_TRUE(parse(R"({"blocks": [{"type": "menu", "items": ["page:My Comics", "settings"]}]})", l));
  ASSERT_EQ(l.blocks[0].count, 2);
  EXPECT_EQ(l.entries[0].action, TPH::Action::Page);
  EXPECT_STREQ(l.entries[0].path, "My Comics");
}

TEST(ThemePackHome, ParsesAUserPage) {
  TPH::Layout l;
  ASSERT_TRUE(parsePage(R"({"title": "Comic shelf", "style": "grid", "columns": 3, "extra": 1,
      "items": ["folder:/Comics", "book:/Comics/a.cbz", "page:Manga", "nope", "library"]})",
                        l));
  ASSERT_EQ(l.blockCount, 1);
  EXPECT_EQ(l.blocks[0].type, TPH::BlockType::Menu);
  EXPECT_EQ(l.blocks[0].style, TPH::Style::Grid);
  EXPECT_EQ(l.blocks[0].columns, 3);
  ASSERT_EQ(l.blocks[0].count, 4);  // no Settings appended: a page has Back
  EXPECT_EQ(l.entries[2].action, TPH::Action::Page);
  EXPECT_STREQ(l.entries[2].path, "Manga");
  EXPECT_EQ(l.entries[3].action, TPH::Action::Library);
  EXPECT_STREQ(l.texts[0], "Comic shelf");
}

TEST(ThemePackHome, PageTitleDefaultsToItsId) {
  TPH::Layout l;
  ASSERT_TRUE(parsePage(R"({"items": ["files"]})", l));
  EXPECT_EQ(l.blocks[0].style, TPH::Style::List);
  EXPECT_STREQ(l.texts[0], "Comics");
  ASSERT_TRUE(parsePage(R"({"title": "", "items": ["files"]})", l));
  EXPECT_STREQ(l.texts[0], "Comics");
}

TEST(ThemePackHome, RejectsPagesWithNothingUsable) {
  TPH::Layout l;
  EXPECT_FALSE(parsePage(R"({"title": "Empty"})", l));
  EXPECT_FALSE(parsePage(R"({"items": ["nope", "folder:relative"]})", l));
  EXPECT_FALSE(parsePage(R"({"items": "library"})", l));
}

namespace {

bool parseFile(const char* path, const bool page, TPH::Layout& out) {
  std::ifstream in(std::string(REPO_ROOT_DIR) + "/" + path);
  EXPECT_TRUE(in) << path;
  JsonDocument doc;
  EXPECT_FALSE(deserializeJson(doc, in)) << path;
  return page ? TPH::parsePage(doc.as<JsonObjectConst>(), path, "Reading", out)
              : TPH::parse(doc.as<JsonObjectConst>(), path, out);
}

}  // namespace

// The sample packs in docs/themes parse with no logged problem.
TEST(ThemePackHome, SamplePacksParseCleanly) {
  const int before = gLogErrors;
  TPH::Layout l;
  ASSERT_TRUE(parseFile("docs/themes/bold/home.json", false, l));
  EXPECT_EQ(l.blockCount, 4);
  ASSERT_TRUE(parseFile("docs/themes/shelf/home.json", false, l));
  EXPECT_EQ(l.imageCount, 1);
  EXPECT_EQ(l.entries[2].action, TPH::Action::Page);
  EXPECT_STREQ(l.entries[2].path, "Reading");
  ASSERT_TRUE(parseFile("docs/themes/shelf/pages/Reading.json", true, l));
  EXPECT_STREQ(l.texts[0], "Reading");
  EXPECT_EQ(gLogErrors, before);
}

TEST(ThemePackHome, ProblemSinkCollectsLinesWhileSet) {
  std::string sink;
  TPH::setProblemSink(&sink);
  TPH::Layout l;
  EXPECT_TRUE(parse(R"({"blocks": [{"type": "weather"}, {"type": "header"}]})", l));
  TPH::setProblemSink(nullptr);
  const std::string expected = "home.json: block 1: type \"weather\" unknown, ignored\n";
  EXPECT_EQ(sink, expected);
  EXPECT_TRUE(parse(R"({"blocks": [{"type": "weather"}, {"type": "header"}]})", l));
  EXPECT_EQ(sink, expected);  // nothing added once the sink is cleared
}
