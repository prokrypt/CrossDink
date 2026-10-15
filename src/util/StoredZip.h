#pragma once

#include <cstddef>
#include <cstdint>

// Headers for an uncompressed ("stored") ZIP: local header + raw bytes per
// entry, then one central header per entry and the end record. The caller
// writes the bytes and supplies each entry's CRC-32 (crc32() below) and size.
namespace StoredZip {

constexpr size_t LOCAL_HEADER = 30;    // plus the name
constexpr size_t CENTRAL_HEADER = 46;  // plus the name
constexpr size_t END_RECORD = 22;

struct Entry {
  const char* name;
  uint32_t crc;
  uint32_t size;
  uint32_t offset;  // of its local header
};

// Running CRC-32 (start at 0). Bitwise, no table: a report is under 100 KB.
uint32_t crc32(uint32_t crc, const uint8_t* data, size_t len);

// Each writes into out (room for the header plus the name) and returns the length.
size_t localHeader(const Entry& e, uint8_t* out);
size_t centralHeader(const Entry& e, uint8_t* out);
size_t endRecord(uint16_t count, uint32_t centralSize, uint32_t centralOffset, uint8_t* out);

}  // namespace StoredZip
