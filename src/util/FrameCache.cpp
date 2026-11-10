#include "FrameCache.h"

#include <Logging.h>
#include <ZipFile.h>

#include <cstdio>
#include <cstring>

#include "AppVersion.h"

namespace {

// Boot frames share the sleep cache folder and its file cap.
constexpr char FRAME_CACHE_DIR[] = "/.crossdink/sleepcache";
constexpr int FRAME_CACHE_MAX_FILES = 128;

// Hashes 16 chunks spread over the file, first and last included: mtime is a
// fixed fallback while the clock is unset, so a same-size replacement needs this.
// shortcut: a sampled hash misses an edit that touches none of the chunks; hash the whole file if that shows up.
uint64_t sampledContentHash(FsFile& file) {
  constexpr int kChunks = 16;
  constexpr size_t kChunkBytes = 128;
  const size_t restore = file.position();
  const size_t size = file.fileSize();
  uint64_t hash = 0;
  char chunk[kChunkBytes];
  for (int i = 0; i < kChunks; ++i) {
    const size_t offset = size > kChunkBytes ? (size - kChunkBytes) / (kChunks - 1) * i : 0;
    if (!file.seekSet(offset)) break;
    const int n = file.read(chunk, kChunkBytes);
    if (n <= 0) break;
    hash = hash * 1099511628211ull ^ ZipFile::fnvHash64(chunk, static_cast<size_t>(n));
  }
  file.seekSet(restore);
  return hash;
}

// Drops the oldest file once the folder holds FRAME_CACHE_MAX_FILES.
// shortcut: oldest by FAT creation time, ties (no clock) drop an arbitrary file; fine for a cache.
void trimFrameCache() {
  auto dir = Storage.open(FRAME_CACHE_DIR);
  if (!dir || !dir.isDirectory()) return;
  int files = 0;
  uint32_t oldestTime = UINT32_MAX;
  char name[32];
  char oldest[32] = "";
  for (auto entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    if (!entry.isDirectory()) {
      ++files;
      const uint32_t created = entry.creationTime();
      if (created < oldestTime && entry.getName(name, sizeof(name)) > 0) {
        oldestTime = created;
        memcpy(oldest, name, sizeof(oldest));
      }
    }
    entry.close();
  }
  dir.close();
  if (files < FRAME_CACHE_MAX_FILES || oldest[0] == '\0') return;
  const std::string victim = std::string(FRAME_CACHE_DIR) + "/" + oldest;
  Storage.remove(victim.c_str());
}

}  // namespace

std::string frameCachePath(const std::string& sourcePath, FsFile& file, const char* variant) {
  char identity[64];
  snprintf(identity, sizeof(identity), "|%016llx|%lu|%lu|", static_cast<unsigned long long>(sampledContentHash(file)),
           static_cast<unsigned long>(file.fileSize()), static_cast<unsigned long>(file.modificationTime()));
  const std::string key = sourcePath + identity + variant + "|" + AppVersion::version();
  char path[64];
  snprintf(path, sizeof(path), "%s/%016llx.bin", FRAME_CACHE_DIR,
           static_cast<unsigned long long>(ZipFile::fnvHash64(key.data(), key.size())));
  return path;
}

bool readFrameCache(const std::string& path, uint8_t* const* buffers, int count, size_t bytes) {
  if (!Storage.exists(path.c_str())) return false;
  FsFile file;
  if (!Storage.openFileForRead("CACHE", path, file)) return false;
  bool ok = file.fileSize() == count * bytes;
  for (int i = 0; ok && i < count; ++i) ok = file.read(buffers[i], bytes) == static_cast<int>(bytes);
  file.close();
  if (!ok) {
    LOG_ERR("CACHE", "Bad frame cache file %s, removing", path.c_str());
    Storage.remove(path.c_str());
  }
  return ok;
}

void writeFrameCache(const std::string& path, const uint8_t* const* buffers, int count, size_t bytes) {
  if (!Storage.ensureDirectoryExists(FRAME_CACHE_DIR)) return;
  trimFrameCache();
  FsFile file;
  if (!Storage.openFileForWrite("CACHE", path, file)) return;
  bool ok = true;
  for (int i = 0; ok && i < count; ++i) ok = file.write(buffers[i], bytes) == bytes;
  file.close();
  if (!ok) {
    LOG_ERR("CACHE", "Failed to write frame cache %s", path.c_str());
    Storage.remove(path.c_str());
  }
}
