#include "StoredZip.h"

#include <cstring>

namespace StoredZip {
namespace {

constexpr uint16_t DOS_DATE_1980_01_01 = (1 << 5) | 1;

uint8_t* put16(uint8_t* p, const uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  return p + 2;
}

uint8_t* put32(uint8_t* p, const uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
  return p + 4;
}

// version, flags, method (0 = stored), time, date, crc, sizes, name length:
// shared by the local and central headers.
uint8_t* common(uint8_t* p, const Entry& e, const uint16_t nameLen) {
  p = put16(p, 10);
  p = put16(p, 0);
  p = put16(p, 0);
  p = put16(p, 0);
  p = put16(p, DOS_DATE_1980_01_01);
  p = put32(p, e.crc);
  p = put32(p, e.size);
  p = put32(p, e.size);
  return put16(p, nameLen);
}

}  // namespace

uint32_t crc32(uint32_t crc, const uint8_t* data, const size_t len) {
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

size_t localHeader(const Entry& e, uint8_t* out) {
  const auto nameLen = static_cast<uint16_t>(strlen(e.name));
  uint8_t* p = put32(out, 0x04034b50);
  p = common(p, e, nameLen);
  p = put16(p, 0);  // extra length
  memcpy(p, e.name, nameLen);
  return LOCAL_HEADER + nameLen;
}

size_t centralHeader(const Entry& e, uint8_t* out) {
  const auto nameLen = static_cast<uint16_t>(strlen(e.name));
  uint8_t* p = put32(out, 0x02014b50);
  p = put16(p, 10);  // made by
  p = common(p, e, nameLen);
  p = put16(p, 0);  // extra length
  p = put16(p, 0);  // comment length
  p = put16(p, 0);  // disk
  p = put16(p, 0);  // internal attributes
  p = put32(p, 0);  // external attributes
  p = put32(p, e.offset);
  memcpy(p, e.name, nameLen);
  return CENTRAL_HEADER + nameLen;
}

size_t endRecord(const uint16_t count, const uint32_t centralSize, const uint32_t centralOffset, uint8_t* out) {
  uint8_t* p = put32(out, 0x06054b50);
  p = put16(p, 0);
  p = put16(p, 0);
  p = put16(p, count);
  p = put16(p, count);
  p = put32(p, centralSize);
  p = put32(p, centralOffset);
  put16(p, 0);  // comment length
  return END_RECORD;
}

}  // namespace StoredZip
