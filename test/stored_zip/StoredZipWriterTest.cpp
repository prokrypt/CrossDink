#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "StoredZip.h"

namespace {

uint32_t read32(const std::vector<uint8_t>& b, const size_t at) {
  return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}
uint16_t read16(const std::vector<uint8_t>& b, const size_t at) { return b[at] | (b[at + 1] << 8); }

// Two entries the way BugReport writes them: local header + bytes each, then
// the central directory and the end record.
std::vector<uint8_t> build() {
  const char* hello = "hello";
  StoredZip::Entry entries[2] = {{"report.txt", 0x3610A686, 5, 0}, {"log.txt", 0, 0, 0}};
  std::vector<uint8_t> zip;
  uint8_t header[64];
  for (auto& e : entries) {
    e.offset = static_cast<uint32_t>(zip.size());
    const size_t n = StoredZip::localHeader(e, header);
    zip.insert(zip.end(), header, header + n);
    if (e.size) zip.insert(zip.end(), hello, hello + e.size);
  }
  const auto centralOffset = static_cast<uint32_t>(zip.size());
  for (const auto& e : entries) {
    const size_t n = StoredZip::centralHeader(e, header);
    zip.insert(zip.end(), header, header + n);
  }
  const auto centralSize = static_cast<uint32_t>(zip.size() - centralOffset);
  const size_t n = StoredZip::endRecord(2, centralSize, centralOffset, header);
  zip.insert(zip.end(), header, header + n);
  return zip;
}

}  // namespace

TEST(StoredZip, Crc32MatchesZlib) {
  const auto* hello = reinterpret_cast<const uint8_t*>("hello");
  EXPECT_EQ(StoredZip::crc32(0, hello, 5), 0x3610A686u);
  // Running over chunks gives the same value.
  EXPECT_EQ(StoredZip::crc32(StoredZip::crc32(0, hello, 2), hello + 2, 3), 0x3610A686u);
  EXPECT_EQ(StoredZip::crc32(0, hello, 0), 0u);
}

TEST(StoredZip, LayoutMatchesTheZipSpec) {
  const std::vector<uint8_t> zip = build();
  ASSERT_EQ(zip.size(), (30 + 10 + 5) + (30 + 7) + (46 + 10) + (46 + 7) + 22u);

  EXPECT_EQ(read32(zip, 0), 0x04034b50u);
  EXPECT_EQ(read16(zip, 8), 0u);  // stored
  EXPECT_EQ(read32(zip, 14), 0x3610A686u);
  EXPECT_EQ(read32(zip, 18), 5u);
  EXPECT_EQ(read32(zip, 22), 5u);
  EXPECT_EQ(memcmp(zip.data() + 30, "report.txthello", 15), 0);

  const size_t end = zip.size() - 22;
  EXPECT_EQ(read32(zip, end), 0x06054b50u);
  EXPECT_EQ(read16(zip, end + 10), 2u);
  const uint32_t centralOffset = read32(zip, end + 16);
  EXPECT_EQ(centralOffset, 45u + 37u);
  EXPECT_EQ(read32(zip, end + 12), end - centralOffset);

  // Second central header points back at the second local header.
  const size_t second = centralOffset + 46 + 10;
  EXPECT_EQ(read32(zip, second), 0x02014b50u);
  EXPECT_EQ(read32(zip, second + 42), 45u);
  EXPECT_EQ(read32(zip, read32(zip, second + 42)), 0x04034b50u);

  if (const char* dump = std::getenv("STORED_ZIP_DUMP")) {
    if (FILE* f = fopen(dump, "wb")) {
      fwrite(zip.data(), 1, zip.size(), f);
      fclose(f);
    }
  }
}
