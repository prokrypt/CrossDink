#include "RecentBookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "util/InPlaceFileWrite.h"

namespace {
constexpr uint32_t EPUB_PERCENT_CACHE_MAGIC = 0x45505250;  // "EPRP"
constexpr uint8_t EPUB_PERCENT_CACHE_VERSION = 1;
constexpr char EPUB_PERCENT_CACHE_FILE[] = "/progress_percent.bin";
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 4;

float clampProgressPercent(const float progress) { return std::clamp(progress, 0.0f, 100.0f); }

std::string epubPercentCachePath(const std::string& cachePath) { return cachePath + EPUB_PERCENT_CACHE_FILE; }

bool readCachedEpubBasisPoints(const std::string& cachePath, uint16_t& basisPoints) {
  if (cachePath.empty()) {
    return false;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBPR", epubPercentCachePath(cachePath), file)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  const bool readOk = serialization::tryReadPod(file, magic) && serialization::tryReadPod(file, version) &&
                      serialization::tryReadPod(file, basisPoints);
  file.close();
  return readOk && magic == EPUB_PERCENT_CACHE_MAGIC && version == EPUB_PERCENT_CACHE_VERSION && basisPoints <= 10000;
}

float loadCachedEpubPercentFromCachePath(const std::string& cachePath) {
  uint16_t basisPoints = 0;
  if (!readCachedEpubBasisPoints(cachePath, basisPoints)) {
    return -1.0f;
  }
  return static_cast<float>(basisPoints) / 100.0f;
}

void saveCachedEpubPercentToCachePath(const std::string& cachePath, const float progress) {
  if (cachePath.empty() || progress < 0.0f) {
    return;
  }

  const float clamped = clampProgressPercent(progress);
  const uint16_t basisPoints = static_cast<uint16_t>((clamped * 100.0f) + 0.5f);

  // Home and the sleep screen recompute this on every visit; a read is cheaper
  // on the card than rewriting an identical value.
  uint16_t existing = 0;
  if (readCachedEpubBasisPoints(cachePath, existing) && existing == basisPoints) {
    return;
  }

  // Same bytes tryWritePod() produced, written over the existing file: a torn
  // write only costs a recompute, since the reader validates magic, version and range.
  uint8_t record[sizeof(EPUB_PERCENT_CACHE_MAGIC) + sizeof(EPUB_PERCENT_CACHE_VERSION) + sizeof(basisPoints)];
  memcpy(record, &EPUB_PERCENT_CACHE_MAGIC, sizeof(EPUB_PERCENT_CACHE_MAGIC));
  memcpy(record + sizeof(EPUB_PERCENT_CACHE_MAGIC), &EPUB_PERCENT_CACHE_VERSION, sizeof(EPUB_PERCENT_CACHE_VERSION));
  memcpy(record + sizeof(EPUB_PERCENT_CACHE_MAGIC) + sizeof(EPUB_PERCENT_CACHE_VERSION), &basisPoints,
         sizeof(basisPoints));
  if (!writeFileInPlace("RBPR", epubPercentCachePath(cachePath).c_str(), record, sizeof(record))) {
    LOG_ERR("RBPR", "failed to write EPUB percent cache: %s", cachePath.c_str());
  }
}

float loadEpubSizeProgressPercentFromCachePath(const std::string& cachePath) {
  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgressFromCachePath("RBPR", cachePath, progress)) {
    return -1.0f;
  }
  if (!progress.hasPageCount || progress.pageCount <= 0) {
    return -1.0f;
  }

  float progressPercent = -1.0f;
  {
    BookMetadataCache metadata(cachePath);
    if (!metadata.load()) {
      return -1.0f;
    }
    if (progress.spineIndex < 0 || progress.spineIndex >= metadata.getSpineCount()) {
      return -1.0f;
    }

    const size_t bookSize = metadata.getSpineCumulativeSize(metadata.getSpineCount() - 1);
    if (bookSize == 0) {
      return -1.0f;
    }

    const size_t prevChapterSize =
        progress.spineIndex >= 1 ? metadata.getSpineCumulativeSize(progress.spineIndex - 1) : 0;
    const size_t currentChapterSize = metadata.getSpineCumulativeSize(progress.spineIndex) - prevChapterSize;
    const float chapterProgress =
        std::clamp(static_cast<float>(progress.pageNumber + 1) / static_cast<float>(progress.pageCount), 0.0f, 1.0f);
    const float totalProgress =
        static_cast<float>(prevChapterSize) + (static_cast<float>(currentChapterSize) * chapterProgress);
    progressPercent = clampProgressPercent((totalProgress / static_cast<float>(bookSize)) * 100.0f);
  }
  saveCachedEpubPercentToCachePath(cachePath, progressPercent);
  return progressPercent;
}

float loadEpubProgressPercent(const RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return -1.0f;
  }

  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgress(epub, progress, "RBPR") || !progress.hasPageCount) {
    return -1.0f;
  }

  if (progress.pageCount <= 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(progress.pageNumber + 1) / static_cast<float>(progress.pageCount);
  const float progressPercent =
      clampProgressPercent(epub.calculateProgress(progress.spineIndex, chapterProgress) * 100.0f);
  saveCachedEpubPercentToCachePath(epub.getCachePath(), progressPercent);
  return progressPercent;
}

float loadXtcProgressPercent(const RecentBook& book) {
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load()) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBPR", xtc.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[4];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return clampProgressPercent(static_cast<float>(xtc.calculateProgress(currentPage)));
}

float loadTxtProgressPercent(const RecentBook& book) {
  Txt txt(book.path, "/.crosspoint");
  if (!txt.load()) {
    return -1.0f;
  }

  FsFile progressFile;
  if (!Storage.openFileForRead("RBPR", txt.getCachePath() + "/progress.bin", progressFile)) {
    return -1.0f;
  }

  // TXT progress starts with a 16-bit page number; the remaining bytes are a
  // file offset used only to restore the reader page.
  // Read the reader's four-byte minimum so an interrupted write is rejected,
  // then decode only its two-byte page field.
  uint8_t progressData[4];
  const int progressBytes = progressFile.read(progressData, sizeof(progressData));
  progressFile.close();
  if (progressBytes != static_cast<int>(sizeof(progressData))) {
    return -1.0f;
  }

  const uint16_t currentPage = static_cast<uint16_t>(progressData[0]) | (static_cast<uint16_t>(progressData[1]) << 8);

  FsFile indexFile;
  if (!Storage.openFileForRead("RBPR", txt.getCachePath() + "/index.bin", indexFile)) {
    return -1.0f;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t fileSize = 0;
  int32_t cachedWidth = 0;
  int32_t cachedLines = 0;
  int32_t fontId = 0;
  int32_t verticalMargin = 0;
  int32_t horizontalMargin = 0;
  uint8_t alignment = 0;
  uint32_t totalPages = 0;
  const bool readOk =
      serialization::tryReadPod(indexFile, magic) && serialization::tryReadPod(indexFile, version) &&
      serialization::tryReadPod(indexFile, fileSize) && serialization::tryReadPod(indexFile, cachedWidth) &&
      serialization::tryReadPod(indexFile, cachedLines) && serialization::tryReadPod(indexFile, fontId) &&
      serialization::tryReadPod(indexFile, verticalMargin) && serialization::tryReadPod(indexFile, horizontalMargin) &&
      serialization::tryReadPod(indexFile, alignment) && serialization::tryReadPod(indexFile, totalPages);
  indexFile.close();
  if (!readOk) {
    return -1.0f;
  }
  (void)cachedWidth;
  (void)cachedLines;
  (void)fontId;
  (void)verticalMargin;
  (void)horizontalMargin;
  (void)alignment;

  if (magic != TXT_CACHE_MAGIC || version != TXT_CACHE_VERSION || fileSize != txt.getFileSize() || totalPages == 0) {
    return -1.0f;
  }

  return clampProgressPercent((static_cast<float>(currentPage + 1) / static_cast<float>(totalPages)) * 100.0f);
}
}  // namespace

float RecentBookProgress::loadPercent(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return loadEpubProgressPercent(book);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return loadXtcProgressPercent(book);
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return loadTxtProgressPercent(book);
  }
  return -1.0f;
}

float RecentBookProgress::loadCachedEpubPercent(const RecentBook& book) {
  if (!FsHelpers::hasEpubExtension(book.path)) {
    return -1.0f;
  }
  const std::string cachePath = Epub::resolveCachePathForFilePath(book.path, "/.crosspoint");
  const float cachedProgress = loadCachedEpubPercentFromCachePath(cachePath);
  if (cachedProgress >= 0.0f) {
    return cachedProgress;
  }
  return loadEpubSizeProgressPercentFromCachePath(cachePath);
}

void RecentBookProgress::saveCachedEpubPercent(const std::string& cachePath, const float progress) {
  saveCachedEpubPercentToCachePath(cachePath, progress);
}

void RecentBookProgress::saveCachedEpubPercent(const Epub& epub, const int spineIndex, const int currentPage,
                                               const int pageCount) {
  if (pageCount <= 0) {
    return;
  }

  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(pageCount);
  const float progressPercent = clampProgressPercent(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f);
  saveCachedEpubPercentToCachePath(epub.getCachePath(), progressPercent);
}

bool RecentBookProgress::hasPercent(const float progress) { return progress >= 0.0f; }

std::string RecentBookProgress::formatPercent(const float progress) {
  if (!hasPercent(progress)) {
    return "";
  }
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%.0f%%", clampProgressPercent(progress));
  return buffer;
}
