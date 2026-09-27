#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "ProgressRecord.h"
#include "util/InPlaceFileWrite.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
  uint32_t visibleTextOffset = 0;
  bool hasVisibleTextOffset = false;
};

inline Progress toProgress(const progress_record::Fields& fields) {
  Progress progress;
  progress.spineIndex = fields.spineIndex;
  progress.pageNumber = fields.pageNumber == UINT16_MAX ? 0 : fields.pageNumber;
  progress.pageCount = fields.hasPageCount ? fields.pageCount : 0;
  progress.hasPageCount = fields.hasPageCount;
  progress.visibleTextOffset = fields.hasVisibleTextOffset ? fields.visibleTextOffset : 0;
  progress.hasVisibleTextOffset = fields.hasVisibleTextOffset;
  return progress;
}

inline progress_record::Slot readProgressSlot(const char* moduleName, const std::string& path) {
  progress_record::Slot slot;
  if (!Storage.exists(path.c_str())) {
    return slot;
  }

  FsFile f;
  if (!Storage.openFileForRead(moduleName, path, f)) {
    return slot;
  }
  uint8_t data[progress_record::RECORD_BYTES];
  const size_t fileSize = f.fileSize();
  const int dataSize = f.read(data, sizeof(data));
  f.close();
  if (dataSize < 0 || static_cast<size_t>(dataSize) != std::min(fileSize, sizeof(data))) {
    LOG_ERR(moduleName, "Short read of progress file %s", path.c_str());
    return slot;
  }
  slot = progress_record::decode(data, fileSize);
  if (slot.kind == progress_record::SlotKind::Invalid) {
    LOG_ERR(moduleName, "Progress file %s is damaged or has unexpected size: %u", path.c_str(),
            static_cast<unsigned>(fileSize));
  }
  return slot;
}

inline std::string progressPrimaryPath(const std::string& cachePath) { return cachePath + "/progress.bin"; }
inline std::string progressBackupPath(const std::string& cachePath) { return cachePath + "/progress.bin.bak"; }

// Reads the current position from the two progress slots of an EPUB cache directory.
inline bool loadProgressFromCachePath(const char* moduleName, const std::string& cachePath, Progress& progress) {
  const progress_record::Slot primary = readProgressSlot(moduleName, progressPrimaryPath(cachePath));
  const progress_record::Slot backup = readProgressSlot(moduleName, progressBackupPath(cachePath));
  switch (progress_record::newest(primary, backup)) {
    case progress_record::PRIMARY:
      progress = toProgress(primary.fields);
      return true;
    case progress_record::BACKUP:
      progress = toProgress(backup.fields);
      return true;
    case progress_record::NONE:
      break;
  }
  return false;
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  return loadProgressFromCachePath(moduleName, epub.getCachePath(), progress);
}

// Persists reader progress to an EPUB cache directory. Returns true on success,
// including when the stored position already matches and nothing is written.
inline bool saveProgressToCachePath(const std::string& cachePath, int spineIndex, int pageNumber, int pageCount,
                                    const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  if (spineIndex < 0 || spineIndex > 0xFFFF || pageNumber < 0 || pageNumber > 0xFFFF || pageCount < 0 ||
      pageCount > 0xFFFF) {
    LOG_ERR("ERS", "Progress values out of range: spine=%d page=%d count=%d", spineIndex, pageNumber, pageCount);
    return false;
  }
  progress_record::Fields fields;
  fields.spineIndex = static_cast<uint16_t>(spineIndex);
  fields.pageNumber = static_cast<uint16_t>(pageNumber);
  fields.pageCount = static_cast<uint16_t>(pageCount);
  fields.hasPageCount = true;
  fields.hasVisibleTextOffset = visibleTextOffset.has_value();
  fields.visibleTextOffset = visibleTextOffset.value_or(0);

  const std::string primaryPath = progressPrimaryPath(cachePath);
  const std::string backupPath = progressBackupPath(cachePath);
  const progress_record::Slot primary = readProgressSlot("ERS", primaryPath);
  const progress_record::Slot backup = readProgressSlot("ERS", backupPath);

  // Reader exit, sync screens and relayouts often re-save the position already on
  // the card; two small reads are far cheaper than a write.
  const progress_record::SlotIndex current = progress_record::newest(primary, backup);
  if (current != progress_record::NONE &&
      progress_record::sameFields(current == progress_record::PRIMARY ? primary.fields : backup.fields, fields)) {
    return true;
  }

  uint8_t record[progress_record::RECORD_BYTES];
  progress_record::encode(fields, progress_record::nextSeq(primary, backup), record);
  const std::string& targetPath =
      progress_record::target(primary, backup) == progress_record::PRIMARY ? primaryPath : backupPath;
  if (!writeFileInPlace("ERS", targetPath.c_str(), record, sizeof(record))) {
    LOG_ERR("ERS", "Could not save progress to %s", targetPath.c_str());
    return false;
  }
  return true;
}

inline bool saveProgress(const Epub& epub, int spineIndex, int pageNumber, int pageCount,
                         const std::optional<uint32_t> visibleTextOffset = std::nullopt) {
  return saveProgressToCachePath(epub.getCachePath(), spineIndex, pageNumber, pageCount, visibleTextOffset);
}

}  // namespace EpubReaderUtils
