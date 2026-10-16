#pragma once

#include <Print.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

class HalFile : public Print {
 public:
  // In-memory contents for tests that read through a file; empty acts as a closed file.
  std::string data;
  size_t pos = 0;
  int available() const { return pos < data.size() ? static_cast<int>(data.size() - pos) : 0; }
  size_t position() const { return pos; }
  size_t size() const { return data.size(); }
  int read(void* out, size_t n) {
    n = std::min(n, data.size() - std::min(pos, data.size()));
    std::memcpy(out, data.data() + pos, n);
    pos += n;
    return static_cast<int>(n);
  }
  bool seek(size_t p) {
    if (data.empty() || p > data.size()) return false;
    pos = p;
    return true;
  }
  bool seekSet(size_t p) { return seek(p); }
  bool close() { return true; }
  bool isOpen() const { return false; }
  operator bool() const { return false; }
  size_t write(uint8_t) override { return 0; }
  size_t write(const uint8_t*, size_t) { return 0; }
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }
  bool openFileForRead(const char*, const std::string&, HalFile&) { return false; }
  bool openFileForWrite(const char*, const std::string&, HalFile&) { return false; }
  bool remove(const char*) { return false; }
  bool exists(const char*) const { return false; }
  bool rename(const char*, const char*) { return false; }
};

#define Storage HalStorage::getInstance()
