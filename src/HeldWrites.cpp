#include "HeldWrites.h"

#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <uzlib.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "CrossPointSettings.h"
#include "util/FileContentEquals.h"
#include "util/InPlaceFileWrite.h"

#ifndef SIMULATOR
#include <sdkconfig.h>
#endif
#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY && !defined(SIMULATOR)
#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_system.h>
#define HELD_IN_PSRAM 1
#else
#define HELD_IN_PSRAM 0
#endif

namespace {

// settings.json ~6 KB, recent.json (18 books) ~5 KB. Without PSRAM noinit every
// hold writes through, and a record only keeps a failed write for a retry.
constexpr size_t kCap = HELD_IN_PSRAM ? 16 * 1024 : 8 * 1024;
constexpr uint32_t kMagic = 0x484C4431;  // "HLD1"; change it when the record layout changes

// Writers get NUL-terminated JSON (JSON never holds a NUL byte).
bool writePlain(const char* path, const char* data, size_t) {
  Storage.mkdir("/.crossdink");
  if (!Storage.writeFile(path, String(data))) {
    LOG_ERR("HLD", "Failed to write %s", path);
    return false;
  }
  return true;
}

bool writeAtomic(const char* path, const char* data, size_t) {
  return PersistableStoreBase::writeStringToFileAtomically(path, String(data));
}

bool writeSettings(const char*, const char* data, const size_t len) {
  return CrossPointSettings::writeJsonToCard(data, len);
}

struct SlotInfo {
  const char* path;
  bool (*write)(const char* path, const char* data, size_t len);
};
// Same order as HeldWrites::Slot.
constexpr SlotInfo kSlots[] = {
    {"/.crossdink/state.json", writePlain},
    {"/.crossdink/recent.json", writeAtomic},
    {"/.crossdink/settings.json", writeSettings},
};
constexpr size_t kCount = static_cast<size_t>(HeldWrites::Slot::Count);
static_assert(sizeof(kSlots) / sizeof(kSlots[0]) == kCount, "one SlotInfo per Slot");

struct Record {
  uint32_t magic;  // kMagic = held, not yet on the card
  uint32_t size;   // sizeof(Record): rejects a record from a build with another layout
  uint32_t len;
  uint32_t crc;
  char data[kCap + 1];  // NUL-terminated
};

// Reader exit files: one book's progress slot and percent per exit, so four
// cover two books exited between flushes.
constexpr size_t kFiles = 4;
constexpr size_t kFilePathCap = 80;          // "/.crossdink/epub_<20 digits>/progress.bin.bak" is 53
constexpr size_t kFileDataCap = 32;          // progress record 20 B, percent 7 B
constexpr uint32_t kFileMagic = 0x484C4631;  // "HLF1"; change it when FileRecord changes

struct FileRecord {
  uint32_t magic;  // kFileMagic = held, not yet on the card
  uint32_t crc;    // over path, len and data
  uint32_t len;
  char path[kFilePathCap];  // NUL-terminated
  uint8_t data[kFileDataCap];
};

struct Held {
  // The S3 MSPI timing tuning overwrites 64 B at physical PSRAM address 0 on
  // every boot, where the noinit segment may start (see PsramRing.h).
  char tuningScratch[1024];
  Record rec[kCount];
  FileRecord files[kFiles];
};

#if HELD_IN_PSRAM
EXT_RAM_NOINIT_ATTR Held held;
#else
Held held;
#endif

std::mutex heldMutex;  // guards held, writeThrough and the card writes of held data
bool ready = false;
uint8_t writeThrough = 0;  // HeldWrites::Reason bits
std::atomic<bool> carryArmed{false};

// ESP.restart() and panics do not write back the PSRAM cache: push each change
// out at once so a crash reset still finds it.
void writeBack(const void* start, const size_t len) {
#if HELD_IN_PSRAM
  esp_cache_msync(const_cast<void*>(start), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#else
  (void)start;
  (void)len;
#endif
}

void clear(Record& r) {
  r.magic = 0;
  writeBack(&r.magic, sizeof(r.magic));
}

uint32_t fileCrc(const FileRecord& f) { return uzlib_crc32(&f.len, sizeof(f) - offsetof(FileRecord, len), 0); }

bool validFile(const FileRecord& f) {
  return f.magic == kFileMagic && f.len <= kFileDataCap && memchr(f.path, '\0', kFilePathCap) != nullptr &&
         fileCrc(f) == f.crc;
}

void clearFile(FileRecord& f) {
  f.magic = 0;
  writeBack(&f.magic, sizeof(f.magic));
}

int findFile(const char* path) {
  for (size_t i = 0; i < kFiles; i++) {
    if (held.files[i].magic == kFileMagic && strcmp(held.files[i].path, path) == 0) return static_cast<int>(i);
  }
  return -1;
}

// Caller holds heldMutex. A cleared cache or a deleted book took the folder:
// writing would only recreate it as an orphan.
bool flushFileLocked(FileRecord& f) {
  char dir[kFilePathCap];
  memcpy(dir, f.path, kFilePathCap);
  if (char* slash = strrchr(dir, '/')) *slash = '\0';
  if (dir[0] != '\0' && !Storage.exists(dir)) {
    LOG_INF("HLD", "Dropping held %s: its folder is gone", f.path);
    clearFile(f);
    return false;
  }
  if (!fileContentEquals("HLD", f.path, f.data, f.len) && !writeFileInPlace("HLD", f.path, f.data, f.len)) {
    LOG_ERR("HLD", "Failed to flush %s; kept for the next flush", f.path);
    return false;
  }
  clearFile(f);
  return true;
}

bool valid(const Record& r) {
  return r.magic == kMagic && r.size == sizeof(Record) && r.len <= kCap && r.data[r.len] == '\0' &&
         uzlib_crc32(r.data, r.len, 0) == r.crc;
}

// Caller holds heldMutex.
bool flushLocked(const size_t i) {
  Record& r = held.rec[i];
  if (!ready || r.magic != kMagic) return true;
  if (!kSlots[i].write(kSlots[i].path, r.data, r.len)) {
    LOG_ERR("HLD", "Failed to flush %s; kept for the next flush", kSlots[i].path);
    return false;
  }
  clear(r);
  return true;
}

int flushFilesLocked() {
  int written = 0;
  for (auto& f : held.files) {
    if (ready && f.magic == kFileMagic && flushFileLocked(f)) written++;
  }
  return written;
}

int flushAllLocked() {
  int written = flushFilesLocked();
  for (size_t i = 0; i < kCount; i++) {
    if (ready && held.rec[i].magic == kMagic && flushLocked(i)) written++;
  }
  if (written) LOG_DBG("HLD", "Flushed %d held file(s)", written);
  return written;
}

bool heldJson(const char* path, String& out) {
  std::lock_guard<std::mutex> lock(heldMutex);
  for (size_t i = 0; i < kCount; i++) {
    if (held.rec[i].magic == kMagic && strcmp(path, kSlots[i].path) == 0) {
      out = held.rec[i].data;
      return true;
    }
  }
  return false;
}

}  // namespace

namespace HeldWrites {

void begin() {
  std::lock_guard<std::mutex> lock(heldMutex);
  if (ready) return;
  ready = true;
#if HELD_IN_PSRAM
  // Power-on PSRAM is random; after a reset (brownout included) the CRC
  // decides, and deep sleep finds nothing held (sleep flushes first).
  const bool kept = esp_reset_reason() != ESP_RST_POWERON;
  for (size_t i = 0; i < kCount; i++) {
    if (kept && valid(held.rec[i])) {
      LOG_INF("HLD", "Carried %s across the restart (%u B)", kSlots[i].path, static_cast<unsigned>(held.rec[i].len));
    } else {
      clear(held.rec[i]);
    }
  }
  for (auto& f : held.files) {
    if (kept && validFile(f)) {
      LOG_INF("HLD", "Carried %s across the restart", f.path);
    } else {
      clearFile(f);
    }
  }
#else
  for (auto& r : held.rec) r.magic = 0;
  for (auto& f : held.files) f.magic = 0;
#endif
  PersistableStoreBase::heldJson = &heldJson;
}

bool hold(const Slot slot, const char* data, const size_t len) {
  const size_t i = static_cast<size_t>(slot);
  std::lock_guard<std::mutex> lock(heldMutex);
  if (!ready) return kSlots[i].write(kSlots[i].path, data, len);  // PSRAM not adopted yet
  Record& r = held.rec[i];
  if (!HELD_IN_PSRAM || writeThrough || len > kCap) {
    if (len > kCap) {
      LOG_ERR("HLD", "%s is %u B, over the %u B slot; writing now", kSlots[i].path, static_cast<unsigned>(len),
              static_cast<unsigned>(kCap));
    }
    if (kSlots[i].write(kSlots[i].path, data, len)) {
      clear(r);  // an older held copy must not land over this one later
      return true;
    }
    if (len > kCap) return false;
    // Hold it instead: the next flush retries the write.
  }
  r.magic = 0;
  memcpy(r.data, data, len);
  r.data[len] = '\0';
  r.size = sizeof(Record);
  r.len = static_cast<uint32_t>(len);
  r.crc = uzlib_crc32(r.data, r.len, 0);
  r.magic = kMagic;
  writeBack(&r, offsetof(Record, data) + len + 1);
  return true;
}

bool flush(const Slot slot) {
  std::lock_guard<std::mutex> lock(heldMutex);
  return flushLocked(static_cast<size_t>(slot));
}

int flushAll() {
  std::lock_guard<std::mutex> lock(heldMutex);
  return flushAllLocked();
}

bool saveFile(const char* path, const uint8_t* data, const size_t len, const bool hold) {
  std::lock_guard<std::mutex> lock(heldMutex);
  const int i = ready ? findFile(path) : -1;
  if (i >= 0 && held.files[i].len == len && memcmp(held.files[i].data, data, len) == 0) return true;
  if (i < 0 && fileContentEquals("HLD", path, data, len)) return true;
  if (hold && ready && HELD_IN_PSRAM && !writeThrough && len <= kFileDataCap && strlen(path) < kFilePathCap) {
    int target = i;
    for (size_t k = 0; k < kFiles && target < 0; k++) {
      if (held.files[k].magic != kFileMagic) target = static_cast<int>(k);
    }
    if (target >= 0) {
      FileRecord& f = held.files[target];
      f.magic = 0;
      memset(f.path, 0, kFilePathCap);
      strcpy(f.path, path);
      f.len = static_cast<uint32_t>(len);
      memcpy(f.data, data, len);
      f.crc = fileCrc(f);
      f.magic = kFileMagic;
      writeBack(&f, sizeof(f));
      return true;
    }
  }
  if (!writeFileInPlace("HLD", path, data, len)) return false;
  if (i >= 0) clearFile(held.files[i]);  // an older held copy must not land over this one later
  return true;
}

int heldFile(const char* path, uint8_t* out, const size_t cap) {
  std::lock_guard<std::mutex> lock(heldMutex);
  const int i = ready ? findFile(path) : -1;
  if (i < 0) return -1;
  const FileRecord& f = held.files[i];
  memcpy(out, f.data, std::min<size_t>(f.len, cap));
  return static_cast<int>(f.len);
}

int readFile(const char* module, const std::string& path, uint8_t* out, const size_t cap) {
  const int n = heldFile(path.c_str(), out, cap);
  if (n >= 0) return std::min<int>(n, static_cast<int>(cap));
  HalFile file;
  if (!Storage.openFileForRead(module, path, file)) return -1;
  const int read = file.read(out, cap);
  file.close();
  return read;
}

void flushFiles() {
  std::lock_guard<std::mutex> lock(heldMutex);
  if (const int written = flushFilesLocked()) LOG_DBG("HLD", "Flushed %d held reader file(s)", written);
}

void setWriteThrough(const Reason reason, const bool on) {
  // One lock for both, so no hold lands between the flush and the switch.
  std::lock_guard<std::mutex> lock(heldMutex);
  const uint8_t was = writeThrough;
  writeThrough = static_cast<uint8_t>(on ? (writeThrough | reason) : (writeThrough & ~reason));
  if (was != writeThrough) LOG_INF("HLD", "Write-through reasons %u -> %u", was, writeThrough);
  if (on) flushAllLocked();
}

void carryNextRestart() { carryArmed.store(true, std::memory_order_relaxed); }

bool carrying() { return carryArmed.load(std::memory_order_relaxed); }

}  // namespace HeldWrites
