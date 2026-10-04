#pragma once

#include <strings.h>

#include <cstdint>
#include <cstring>

// One Range header against a `size`-byte file (RFC 9110 14.1.2). Ignore covers
// a missing, malformed or multi-range header: the reply is then the whole file
// with 200, as the RFC allows.
enum class ByteRange : uint8_t { Ignore, Ok, Unsatisfiable };

namespace http_range_detail {
inline bool parseNumber(const char*& p, uint64_t& value) {
  if (*p < '0' || *p > '9') return false;
  value = 0;
  for (; *p >= '0' && *p <= '9'; ++p) {
    if (value > (UINT64_MAX - 9) / 10) return false;
    value = value * 10 + static_cast<uint64_t>(*p - '0');
  }
  return true;
}
}  // namespace http_range_detail

// On Ok, [first, last] is the inclusive byte span to send.
inline ByteRange parseByteRange(const char* header, const uint64_t size, uint64_t& first, uint64_t& last) {
  using http_range_detail::parseNumber;
  if (!header || strncasecmp(header, "bytes=", 6) != 0 || strchr(header, ',')) return ByteRange::Ignore;
  const char* p = header + 6;
  uint64_t a = 0;
  uint64_t b = 0;
  if (*p == '-') {  // suffix: the last b bytes
    ++p;
    if (!parseNumber(p, b) || *p) return ByteRange::Ignore;
    if (b == 0 || size == 0) return ByteRange::Unsatisfiable;
    first = b < size ? size - b : 0;
    last = size - 1;
    return ByteRange::Ok;
  }
  if (!parseNumber(p, a) || *p++ != '-') return ByteRange::Ignore;
  if (*p) {
    if (!parseNumber(p, b) || *p || b < a) return ByteRange::Ignore;
  } else {
    b = UINT64_MAX;  // open-ended "a-"
  }
  if (a >= size) return ByteRange::Unsatisfiable;
  first = a;
  last = b < size ? b : size - 1;
  return ByteRange::Ok;
}

#ifndef HTTP_RANGE_PARSE_ONLY
class WebServer;
class HalFile;
// Replies with `file` (200, or 206/416 per the request's Range) and streams
// the body. Callers add their own headers (Content-Disposition) first.
void sendFileWithRange(WebServer& server, HalFile& file, const char* contentType);
#endif
