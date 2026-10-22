#pragma once
#include <HalStorage.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// Writes through to the Storage stub; the real hold-until-flush is HeldWrites' own concern.
namespace HeldWrites {
inline bool failSave = false;
inline bool shortRead = false;
inline bool saveFile(const char* path, const uint8_t* data, size_t len, bool) {
  if (failSave) return false;
  Storage.files[path].assign(data, data + len);
  return true;
}
inline int heldFile(const char*, uint8_t*, size_t) { return -1; }
inline int readFile(const char*, const std::string& path, uint8_t* out, size_t cap) {
  if (!Storage.files.contains(path)) return -1;
  const auto& bytes = Storage.files[path];
  size_t n = std::min(cap, bytes.size());
  if (shortRead && n) --n;
  std::memcpy(out, bytes.data(), n);
  return static_cast<int>(n);
}
}  // namespace HeldWrites
