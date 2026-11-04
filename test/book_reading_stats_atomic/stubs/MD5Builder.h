#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

// Only the file-name derivation uses this; any stable digest will do natively.
class MD5Builder {
  uint64_t hash = 1469598103934665603ULL;

 public:
  void begin() { hash = 1469598103934665603ULL; }
  void add(const char* text) {
    for (const char* p = text; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 1099511628211ULL;
  }
  void calculate() {}
  std::string toString() const {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
  }
};
