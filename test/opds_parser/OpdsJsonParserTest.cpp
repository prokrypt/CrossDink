#include <OpdsParser.h>
#include <gtest/gtest.h>

#include <string>

namespace {
class OpdsJsonTest : public ::testing::Test {
 protected:
  OpdsEntry entries[4];
  static bool feed(OpdsParser& parser, const std::string& text) {
    for (const auto byte : text)
      if (parser.write(static_cast<uint8_t>(byte)) != 1) return false;
    parser.flush();
    return bool(parser);
  }
};

TEST_F(OpdsJsonTest, SniffsBothFormatsAndKeepsPaging) {
  OpdsParser parser(entries, 4);
  parser.setPreferredLanguage("EN");
  ASSERT_TRUE(feed(
      parser,
      "\xEF\xBB\xBF \n"
      R"({"metadata":{"numberOfItems":120},"publications":[{"metadata":{"title":{"fr":"Livre","en":"Book"},"author":[{"name":"Writer"}]},"links":[{"href":"sample.epub","type":"application/epub+zip","rel":"http://opds-spec.org/acquisition/sample"},{"href":"book.epub","type":"application/epub+zip","rel":["self","http://opds-spec.org/acquisition/open-access"]}]}],"links":[{"href":"?page=2","rel":"next"},{"href":"?page=0","rel":"previous"},{"href":"?q={searchTerms}","rel":"search","templated":true}]})"));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_EQ(entries[0].type, OpdsEntryType::BOOK);
  EXPECT_STREQ(entries[0].title.c_str(), "Book");
  EXPECT_STREQ(entries[0].author.c_str(), "Writer");
  EXPECT_STREQ(entries[0].href.c_str(), "book.epub");
  EXPECT_FALSE(entries[0].indirect);
  EXPECT_FALSE(entries[0].unsupportedAcquisition);
  EXPECT_EQ(parser.getNextPageUrl(), "?page=2");
  EXPECT_EQ(parser.getPrevPageUrl(), "?page=0");
  EXPECT_EQ(parser.getSearchTemplate(), "?q={searchTerms}");
  EXPECT_EQ(parser.getTotalResults(), 120);
  OpdsParser xml(entries, 4);
  ASSERT_TRUE(
      feed(xml, "\n<feed><entry><title>Folder</title><link href='next' type='application/atom+xml'/></entry></feed>"));
  EXPECT_EQ(xml.getEntryCount(), 1u);
  EXPECT_EQ(xml.getTotalResults(), -1);
}

TEST_F(OpdsJsonTest, ParseWholeDocumentAndClearBetweenFormats) {
  OpdsParser parser(entries, 4);
  const std::string json = R"({"navigation":[{"title":"One","href":"one"}]})";
  ASSERT_TRUE(parser.parse(json.c_str(), json.size()));
  EXPECT_EQ(parser.getEntryCount(), 1u);
  const std::string xml = "<feed><entry><title>A</title><link href='a' type='application/atom+xml'/></entry></feed>";
  ASSERT_TRUE(parser.parse(xml.c_str(), xml.size()));
  EXPECT_EQ(parser.getEntryCount(), 1u);
  EXPECT_STREQ(entries[0].href.c_str(), "a");
}

TEST_F(OpdsJsonTest, BoundsEntriesAndReportsTruncation) {
  OpdsParser parser(entries, 1);
  ASSERT_TRUE(feed(
      parser,
      R"({"navigation":[{"title":"One","href":"one"},{"title":"Two","href":"two"}],"links":[{"rel":"next","href":"next"}]})"));
  EXPECT_EQ(parser.getEntryCount(), 1u);
  EXPECT_TRUE(parser.wasTruncated());
  EXPECT_TRUE(entries[1].title.empty());
}

TEST_F(OpdsJsonTest, GroupsAndFacetsRemainNavigable) {
  OpdsParser parser(entries, 4);
  ASSERT_TRUE(feed(
      parser,
      R"({"groups":[{"metadata":{"title":"All stories"},"links":[{"rel":"self","href":"stories"}],"navigation":[{"title":"Category","href":"category"}]}],"facets":[{"links":[{"title":"English","href":"en","properties":{"numberOfItems":42}}]}]})"));
  ASSERT_EQ(parser.getEntryCount(), 3u);
  EXPECT_STREQ(entries[1].title.c_str(), "All stories");
  EXPECT_STREQ(entries[1].href.c_str(), "stories");
  EXPECT_EQ(entries[2].count, 42);
}

TEST_F(OpdsJsonTest, RejectsTruncatedAndInvalidDocuments) {
  for (const std::string text : {"", "{", "{}{}", "[1]", "oops", "{\"navigation\":[{\"title\":\"unfinished"}) {
    OpdsParser parser(entries, 4);
    EXPECT_FALSE(feed(parser, text)) << text;
  }
}

TEST_F(OpdsJsonTest, StandalonePublicationAndIndirectLinks) {
  OpdsParser parser(entries, 1, true);
  const std::string doc =
      R"({"metadata":{"title":"Details"},"links":[{"rel":"self","type":"application/opds-publication+json","href":"details.json"},{"rel":"acquisition","type":"application/opds-publication+json","href":"fulfill.json"}]})";
  ASSERT_TRUE(parser.parse(doc.c_str(), doc.size()));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_TRUE(entries[0].indirect);
  EXPECT_STREQ(entries[0].href.c_str(), "fulfill.json");
}

TEST_F(OpdsJsonTest, DoesNotTreatLcpOrPurchasesAsEpubDownloads) {
  OpdsParser parser(entries, 4);
  ASSERT_TRUE(feed(
      parser,
      R"({"publications":[{"metadata":{"title":"Loan"},"links":[{"rel":"borrow","type":"application/vnd.readium.lcp.license.v1.0+json","href":"loan.lcpl"}]},{"metadata":{"title":"Purchase"},"links":[{"rel":"buy","type":"application/epub+zip","href":"buy"}]}]})"));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_TRUE(entries[0].unsupportedAcquisition);
  EXPECT_TRUE(entries[1].unsupportedAcquisition);
}

TEST_F(OpdsJsonTest, DecodesUnicodeEscapesInTitles) {
  OpdsParser parser(entries, 4);
  ASSERT_TRUE(feed(parser, R"({"navigation":[{"title":"Café","href":"cafe"}]})"));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  EXPECT_STREQ(entries[0].title.c_str(), "Caf\xC3\xA9");
}
}  // namespace
