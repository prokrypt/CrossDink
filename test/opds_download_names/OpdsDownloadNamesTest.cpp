#include <gtest/gtest.h>

#include <string>

#include "HttpDownloadFilename.h"
#include "OpdsDownloadFolder.h"

TEST(HttpDownloadFilenameTest, QuotedSemicolonsAndUtf8AreExact) {
  std::string name;
  ASSERT_TRUE(HttpDownloadFilename::parse("attachment; FILENAME = \"The Guest List_ A Novel; Lucy.epub\"", name));
  EXPECT_EQ(name, "The Guest List_ A Novel; Lucy.epub");
  ASSERT_TRUE(HttpDownloadFilename::parse("inline; filename=Literal%20Space.epub", name));
  EXPECT_EQ(name, "Literal%20Space.epub");
  ASSERT_TRUE(HttpDownloadFilename::parse("attachment; filename*=utf-8''Cafe%CC%81.epub", name));
  EXPECT_EQ(name, "Cafe\xCC\x81.epub");
  ASSERT_TRUE(HttpDownloadFilename::parse("attachment; filename=Safe.epub; filename*=UNKNOWN''bad", name));
  EXPECT_EQ(name, "Safe.epub");
}

TEST(HttpDownloadFilenameTest, RejectsUnsafeAmbiguousAndMalformedNames) {
  std::string name;
  for (const auto* header :
       {"", "attachment", "attachment; filename=../escape.epub", "attachment; filename=folder\\escape.epub",
        "attachment; filename=.hidden.epub", "attachment; filename=Book:Title.epub", "attachment; filename=Book.epub.",
        "attachment; filename=Book.exe", "attachment; filename=\"unfinished.epub",
        "attachment; filename=a.epub; FILENAME=b.epub", "attachment; filename*=UTF-8''bad%GG.epub",
        "attachment; filename*=UTF-8''bad%00.epub", "attachment; filename*=UTF-8''%C0%AF.epub",
        "attachment; filename*=UTF-8''%ED%A0%80.epub", "attachment; filename*=UTF-8''%F4%90%80%80.epub",
        "attachment; filename*=UTF-8''%2Fescape.epub", "attachment; filename*=UTF-8''..%2F..%2Fx.epub",
        "attachment; filename=bad\r\n.epub"}) {
    EXPECT_FALSE(HttpDownloadFilename::parse(header, name)) << header;
    EXPECT_TRUE(name.empty());
  }
}

TEST(HttpDownloadFilenameTest, RejectsDesktopReservedNames) {
  std::string name;
  for (const auto* value : {"CON.epub", "prn.epub", "NUL .epub", "COM1.epub", "Lpt9.epub"})
    EXPECT_FALSE(HttpDownloadFilename::parse(std::string("attachment; filename=") + value, name)) << value;
  EXPECT_TRUE(HttpDownloadFilename::parse("attachment; filename=COM10.epub", name));
}

TEST(HttpDownloadFilenameTest, LengthLimitsDoNotSilentlyTruncateIdentity) {
  std::string name;
  std::string maximum(235, 'a');
  maximum += ".epub";
  EXPECT_TRUE(HttpDownloadFilename::parse("attachment; filename=" + maximum, name));
  EXPECT_EQ(name, maximum);
  EXPECT_FALSE(HttpDownloadFilename::parse("attachment; filename=a" + maximum, name));
  EXPECT_FALSE(HttpDownloadFilename::parse(std::string(1025, 'a'), name));
}

TEST(OpdsDownloadFolderTest, NormalizesToAbsoluteFolder) {
  std::string folder = "unchanged";
  ASSERT_TRUE(OpdsDownloadFolder::normalize("", folder));
  EXPECT_EQ(folder, "");
  ASSERT_TRUE(OpdsDownloadFolder::normalize(" / ", folder));
  EXPECT_EQ(folder, "/");
  ASSERT_TRUE(OpdsDownloadFolder::normalize("Books//OPDS/", folder));
  EXPECT_EQ(folder, "/Books/OPDS");
  ASSERT_TRUE(OpdsDownloadFolder::normalize("/Books", folder));
  EXPECT_EQ(folder, "/Books");
}

TEST(OpdsDownloadFolderTest, RejectsTraversalInvalidCharsAndLength) {
  for (const auto* input :
       {"..", "/Books/../x", "./x", "Books.", "Bo /x", "a:b", "a*b", "a?b", "a|b", "a\\b", "a\"b", "a<b", "a\tb\x01"}) {
    std::string folder = "kept";
    EXPECT_FALSE(OpdsDownloadFolder::normalize(input, folder)) << input;
    EXPECT_EQ(folder, "kept");
  }
  std::string folder;
  EXPECT_TRUE(OpdsDownloadFolder::normalize(std::string(62, 'a'), folder));  // "/" + 62
  EXPECT_FALSE(OpdsDownloadFolder::normalize(std::string(63, 'a'), folder));
  EXPECT_FALSE(OpdsDownloadFolder::normalize(std::string(64, 'a'), folder));
}

TEST(OpdsDownloadFolderTest, EmptyOverrideInheritsDefault) {
  EXPECT_STREQ(OpdsDownloadFolder::effective("", "/Default"), "/Default");
  const std::string own = "/Own";
  EXPECT_STREQ(OpdsDownloadFolder::effective(own, "/Default"), "/Own");
}
