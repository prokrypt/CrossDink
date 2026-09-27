#include "ReaderProgressShadow.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <uzlib.h>

#include <cstddef>
#include <cstring>

#include "EpubReaderUtils.h"
#include "util/FileContentEquals.h"
#include "util/InPlaceFileWrite.h"

namespace ReaderProgressShadow {
namespace {

constexpr uint32_t RECORD_MAGIC = 0x52505331;  // "RPS1"
constexpr uint8_t RECORD_VERSION = 1;
// Cache paths look like "/.crosspoint/epub_<up to 20 digits>".
constexpr size_t MAX_CACHE_PATH = 64;

struct Record {
  uint32_t magic;
  uint8_t version;
  uint8_t kind;
  uint8_t pending;
  uint8_t reserved;
  uint32_t position;
  uint32_t metadata;
  char cachePath[MAX_CACHE_PATH];
  uint32_t crc;
};

// RTC_NOINIT holds garbage after power-on, so every read goes through isValid().
RTC_NOINIT_ATTR Record rtcRecord;

uint32_t checksum(const Record& record) { return uzlib_crc32(&record, offsetof(Record, crc), 0); }

bool isValid(const Record& record) {
  return record.magic == RECORD_MAGIC && record.version == RECORD_VERSION && record.crc == checksum(record) &&
         memchr(record.cachePath, '\0', MAX_CACHE_PATH) != nullptr;
}

void seal() { rtcRecord.crc = checksum(rtcRecord); }

void clear() {
  rtcRecord.magic = 0;
  rtcRecord.crc = 0;
}

bool writeTxtProgress(const char* cachePath, const uint32_t page, const uint32_t offset) {
  // Same 6-byte layout as TxtReaderActivity::saveProgress: page(2 LE) + offset(4 LE).
  const uint8_t data[6] = {static_cast<uint8_t>(page),         static_cast<uint8_t>(page >> 8),
                           static_cast<uint8_t>(offset),       static_cast<uint8_t>(offset >> 8),
                           static_cast<uint8_t>(offset >> 16), static_cast<uint8_t>(offset >> 24)};
  const std::string path = std::string(cachePath) + "/progress.bin";
  return fileContentEquals("RPS", path.c_str(), data, sizeof(data)) ||
         writeFileInPlace("RPS", path.c_str(), data, sizeof(data));
}

bool writeXtcProgress(const char* cachePath, const uint32_t page) {
  // Same 4-byte layout as XtcReaderActivity::saveProgress.
  const uint8_t data[4] = {static_cast<uint8_t>(page), static_cast<uint8_t>(page >> 8),
                           static_cast<uint8_t>(page >> 16), static_cast<uint8_t>(page >> 24)};
  const std::string path = std::string(cachePath) + "/progress.bin";
  return fileContentEquals("RPS", path.c_str(), data, sizeof(data)) ||
         writeFileInPlace("RPS", path.c_str(), data, sizeof(data));
}

}  // namespace

void recoverPending() {
  if (!isValid(rtcRecord)) {
    clear();
    return;
  }
  if (!rtcRecord.pending) return;

  const char* cachePath = rtcRecord.cachePath;
  const uint32_t position = rtcRecord.position;
  const uint32_t metadata = rtcRecord.metadata;
  // A missing cache directory means a different card or a cleared cache, not
  // this book's; writing there would only recreate an orphaned progress file.
  if (!Storage.exists(cachePath)) {
    LOG_INF("RPS", "Dropping unsaved position for %s: cache directory is gone", cachePath);
    clear();
    return;
  }

  bool saved = false;
  switch (static_cast<Kind>(rtcRecord.kind)) {
    case Kind::Epub:
      saved = EpubReaderUtils::saveProgressToCachePath(cachePath, static_cast<int>(position >> 16),
                                                       static_cast<int>(position & 0xFFFFU),
                                                       static_cast<int>(metadata & 0xFFFFU));
      break;
    case Kind::Txt:
      saved = writeTxtProgress(cachePath, position, metadata);
      break;
    case Kind::Xtc:
      saved = writeXtcProgress(cachePath, position);
      break;
    case Kind::None:
      break;
  }
  if (saved) {
    LOG_INF("RPS", "Recovered unsaved reading position for %s (position=%lu)", cachePath,
            static_cast<unsigned long>(position));
  } else {
    LOG_ERR("RPS", "Could not recover unsaved reading position for %s", cachePath);
  }
  // Retrying a failed write on every boot would not help; the reader rereads
  // progress.bin and carries on from what the card holds.
  clear();
}

bool bind(const Kind kind, const std::string& cachePath) {
  recoverPending();
  if (kind == Kind::None || cachePath.empty() || cachePath.size() >= MAX_CACHE_PATH) {
    clear();
    return false;
  }
  rtcRecord.magic = RECORD_MAGIC;
  rtcRecord.version = RECORD_VERSION;
  rtcRecord.kind = static_cast<uint8_t>(kind);
  rtcRecord.pending = 0;
  rtcRecord.reserved = 0;
  rtcRecord.position = 0;
  rtcRecord.metadata = 0;
  memset(rtcRecord.cachePath, 0, MAX_CACHE_PATH);
  memcpy(rtcRecord.cachePath, cachePath.data(), cachePath.size());
  seal();
  return true;
}

void unbind(const bool flushed) {
  if (flushed || !isValid(rtcRecord) || !rtcRecord.pending) clear();
}

void notePending(const uint32_t position, const uint32_t metadata) {
  if (!isValid(rtcRecord)) return;
  rtcRecord.position = position;
  rtcRecord.metadata = metadata;
  rtcRecord.pending = 1;
  seal();
}

void notePersisted(const uint32_t position, const uint32_t metadata) {
  if (!isValid(rtcRecord) || !rtcRecord.pending) return;
  if (rtcRecord.position != position || rtcRecord.metadata != metadata) return;
  rtcRecord.pending = 0;
  seal();
}

}  // namespace ReaderProgressShadow
