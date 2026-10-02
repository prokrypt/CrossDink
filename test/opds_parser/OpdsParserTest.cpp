#include <OpdsParser.h>
#include <gtest/gtest.h>

namespace {

constexpr char kMultiAuthorFeed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <title>Book Title</title>
    <id>urn:booklore:book:90</id>
    <author><name>Main Author Name</name></author>
    <author><name>Translator Name</name></author>
    <link href="/api/v1/opds/90/download?fileId=691" rel="http://opds-spec.org/acquisition" type="application/epub+zip" title="EPUB"/>
  </entry>
</feed>)";

// Sanitized from a live Mayberry /opds/branches feed.
constexpr char kSummaryCountFeed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:opds="http://opds-spec.org/2010/catalog">
  <id>urn:example:branches</id>
  <title>Branches</title>
  <entry>
    <id>urn:example:branch:a</id>
    <title>Branch A</title>
    <summary type="text">12713 books</summary>
    <link rel="subsection" href="/opds/branch/a" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:branch:b</id>
    <title>Branch B</title>
    <summary type="text">1 books</summary>
    <link rel="subsection" href="/opds/branch/b" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:releases</id>
    <title>New Releases</title>
    <link rel="subsection" href="/opds/releases" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:descr</id>
    <title>Described</title>
    <summary type="text">3 friends set out on a journey</summary>
    <link rel="subsection" href="/opds/descr" type="application/atom+xml;profile=opds-catalog;kind=navigation"></link>
  </entry>
</feed>)";

constexpr char kThrCountFeed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom" xmlns:thr="http://purl.org/syndication/thread/1.0">
  <entry>
    <title>Fiction</title>
    <summary>99 books</summary>
    <link rel="subsection" href="/fiction" type="application/atom+xml;profile=opds-catalog" thr:count="42"/>
  </entry>
  <entry>
    <title>A Book</title>
    <summary>5 stars</summary>
    <link href="/b.epub" rel="http://opds-spec.org/acquisition" type="application/epub+zip" length="1677721"/>
  </entry>
</feed>)";

}  // namespace

TEST(OpdsParserTest, MultipleAuthorsKeepFirstAuthor) {
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kMultiAuthorFeed, sizeof(kMultiAuthorFeed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 1u);
  ASSERT_NE(parser.getEntry(0), nullptr);
  EXPECT_EQ(parser.getEntry(0)->author, "Main Author Name");
}

TEST(OpdsParserTest, NavigationCountFromSummary) {
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kSummaryCountFeed, sizeof(kSummaryCountFeed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 4u);
  EXPECT_EQ(parser.getEntry(0)->count, 12713);
  EXPECT_EQ(parser.getEntry(1)->count, 1);
  EXPECT_EQ(parser.getEntry(2)->count, -1);
  EXPECT_EQ(parser.getEntry(3)->count, -1);
}

TEST(OpdsParserTest, NavigationCountFromIndentedSummary) {
  constexpr char feed[] = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <id>urn:example:indented</id>
    <title>Indented</title>
    <summary type="text">
                        12713 books
                    </summary>
    <link rel="subsection" href="/opds/a" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
  <entry>
    <id>urn:example:sentence</id>
    <title>Sentence</title>
    <summary type="text">
        3 books      that were picked by the staff this week
    </summary>
    <link rel="subsection" href="/opds/b" type="application/atom+xml;profile=opds-catalog;kind=acquisition"></link>
  </entry>
</feed>)";
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(feed, sizeof(feed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_EQ(parser.getEntry(0)->count, 12713);
  EXPECT_EQ(parser.getEntry(1)->count, -1);
}

TEST(OpdsParserTest, ThrCountWinsAndBooksHaveNoCount) {
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kThrCountFeed, sizeof(kThrCountFeed) - 1));
  ASSERT_EQ(parser.getEntryCount(), 2u);
  EXPECT_EQ(parser.getEntry(0)->type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(parser.getEntry(0)->count, 42);
  EXPECT_EQ(parser.getEntry(1)->type, OpdsEntryType::BOOK);
  EXPECT_EQ(parser.getEntry(1)->count, -1);
  EXPECT_EQ(parser.getEntry(0)->length, -1);
  EXPECT_EQ(parser.getEntry(1)->length, 1677721);
}

TEST(OpdsParserTest, OpenSearchDescriptionGivesSearchTemplate) {
  constexpr char kRoot[] = R"(<feed xmlns="http://www.w3.org/2005/Atom">
  <link rel="search" href="/opds/opensearch.xml" type="application/opensearchdescription+xml"/>
</feed>)";
  constexpr char kDescription[] = R"(<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <Url type="text/html" template="/search?q={searchTerms}"/>
  <Url type="application/atom+xml;profile=opds-catalog" template="/opds/search?q={searchTerms}"/>
</OpenSearchDescription>)";
  OpdsEntry entries[MAX_OPDS_FEED_ENTRIES];
  OpdsParser parser(entries);

  ASSERT_TRUE(parser.parse(kRoot, sizeof(kRoot) - 1));
  EXPECT_EQ(parser.getSearchTemplate(), "");
  EXPECT_EQ(parser.getSearchDescriptionUrl(), "/opds/opensearch.xml");

  ASSERT_TRUE(parser.parse(kDescription, sizeof(kDescription) - 1));
  EXPECT_EQ(parser.getSearchTemplate(), "/opds/search?q={searchTerms}");
}
