#pragma once

#include <Arduino.h>

#include <algorithm>
#include <string>
#include <unordered_map>

#ifndef O_RDWR
#define O_RDWR 0x02
#endif
#ifndef O_CREAT
#define O_CREAT 0x40
#endif

// In-place writes only: bytes go straight into the stored file.
class HalFile {
 public:
  HalFile() = default;
  explicit HalFile(std::string* bytes) : bytes_(bytes) {}

  size_t write(const uint8_t* input, const size_t length) {
    if (!bytes_) return 0;
    if (cursor_ + length > bytes_->size()) bytes_->resize(cursor_ + length);
    std::copy_n(input, length, bytes_->begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ += length;
    return length;
  }
  bool seekSet(const size_t position) {
    cursor_ = position;
    return bytes_ != nullptr;
  }
  void flush() {}
  bool sync() const { return bytes_ != nullptr; }
  bool close() {
    bytes_ = nullptr;
    return true;
  }
  size_t fileSize() const { return bytes_ ? bytes_->size() : 0; }
  explicit operator bool() const { return bytes_ != nullptr; }

 private:
  std::string* bytes_ = nullptr;
  size_t cursor_ = 0;
};

class HalStorage {
 public:
  void reset() {
    files.clear();
    failRenameFrom.clear();
  }

  bool mkdir(const char*, bool = true) { return true; }
  bool exists(const char* path) const { return files.contains(path); }
  bool remove(const char* path) { return files.erase(path) > 0; }

  bool rename(const char* from, const char* to) {
    if (failRenameFrom == from) {
      failRenameFrom.clear();
      return false;
    }
    const auto source = files.find(from);
    if (source == files.end() || files.contains(to)) return false;
    files[to] = source->second;
    files.erase(source);
    return true;
  }

  HalFile open(const char* path, int) { return HalFile(&files[path]); }

  bool writeFile(const char* path, const String& content) {
    files[path] = content.c_str();
    return true;
  }

  String readFile(const char* path) const {
    const auto file = files.find(path);
    return file == files.end() ? String() : String(file->second.c_str());
  }

  void put(const std::string& path, std::string content) { files[path] = std::move(content); }
  void failNextRenameFrom(std::string path) { failRenameFrom = std::move(path); }

 private:
  std::unordered_map<std::string, std::string> files;
  std::string failRenameFrom;
};

inline HalStorage Storage;
