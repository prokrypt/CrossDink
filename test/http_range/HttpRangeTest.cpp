#include <gtest/gtest.h>

#include "HttpRange.h"

namespace {
ByteRange parse(const char* h, uint64_t size, uint64_t& first, uint64_t& last) {
  first = last = 12345;
  return parseByteRange(h, size, first, last);
}
}  // namespace

TEST(HttpRange, ParsesSpans) {
  uint64_t f, l;
  EXPECT_EQ(parse("bytes=0-99", 1000, f, l), ByteRange::Ok);
  EXPECT_EQ(f, 0u);
  EXPECT_EQ(l, 99u);
  EXPECT_EQ(parse("bytes=500-", 1000, f, l), ByteRange::Ok);
  EXPECT_EQ(f, 500u);
  EXPECT_EQ(l, 999u);
  EXPECT_EQ(parse("bytes=900-5000", 1000, f, l), ByteRange::Ok);  // end clamps
  EXPECT_EQ(l, 999u);
  EXPECT_EQ(parse("bytes=-100", 1000, f, l), ByteRange::Ok);
  EXPECT_EQ(f, 900u);
  EXPECT_EQ(l, 999u);
  EXPECT_EQ(parse("bytes=-5000", 1000, f, l), ByteRange::Ok);  // suffix longer than file
  EXPECT_EQ(f, 0u);
  EXPECT_EQ(parse("Bytes=999-999", 1000, f, l), ByteRange::Ok);
  EXPECT_EQ(f, 999u);
}

TEST(HttpRange, Unsatisfiable) {
  uint64_t f, l;
  EXPECT_EQ(parse("bytes=1000-", 1000, f, l), ByteRange::Unsatisfiable);
  EXPECT_EQ(parse("bytes=1000-2000", 1000, f, l), ByteRange::Unsatisfiable);
  EXPECT_EQ(parse("bytes=-0", 1000, f, l), ByteRange::Unsatisfiable);
  EXPECT_EQ(parse("bytes=0-", 0, f, l), ByteRange::Unsatisfiable);
  EXPECT_EQ(parse("bytes=-10", 0, f, l), ByteRange::Unsatisfiable);
}

TEST(HttpRange, IgnoresInvalidAndMulti) {
  uint64_t f, l;
  EXPECT_EQ(parse(nullptr, 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("items=0-9", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes=0-9,20-29", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes=9-0", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes=-", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes=a-9", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes= 0-9", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes=0-9x", 1000, f, l), ByteRange::Ignore);
  EXPECT_EQ(parse("bytes=99999999999999999999-", 1000, f, l), ByteRange::Ignore);  // overflow
}
