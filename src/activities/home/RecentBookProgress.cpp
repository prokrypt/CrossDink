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
#include <mutex>
#include <string>

#include "HeldWrites.h"
#include "KOReaderCredentialStore.h"
#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/KOSyncAuto.h"
#include "util/InPlaceFileWrite.h"

using RecentBookProgress::SyncMark;

namespace {
constexpr uint32_t EPUB_PERCENT_CACHE_MAGIC = 0x45505250;  // "EPRP"
// v2 appends the KOSync mark (bp, flags); v1 (7 bytes, CrossInk's too) reads as never checked.
constexpr uint8_t EPUB_PERCENT_CACHE_VERSION = 2;
constexpr uint8_t SYNC_FLAG_SERVER_AHEAD = 1;
constexpr char EPUB_PERCENT_CACHE_FILE[] = "/progress_percent.bin";
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 4;

float clampProgressPercent(const float progress) { return std::clamp(progress, 0.0f, 100.0f); }

std::string epubPercentCachePath(const std::string& cachePath) { return cachePath + EPUB_PERCENT_CACHE_FILE; }

bool readCachedEpubRecord(const std::string& cachePath, uint16_t& basisPoints, SyncMark& mark) {
  mark = SyncMark{};
  if (cachePath.empty()) {
    return false;
  }

  // Magic, version, basis points; v2 appends the KOSync mark (bp, flags).
  constexpr size_t kVersionAt = sizeof(uint32_t);
  constexpr size_t kBasisAt = kVersionAt + sizeof(uint8_t);
  constexpr size_t kV1Size = kBasisAt + sizeof(uint16_t);
  uint8_t record[kV1Size + sizeof(uint16_t) + sizeof(uint8_t)];
  const int got = HeldWrites::readFile("RBPR", epubPercentCachePath(cachePath), record, sizeof(record));
  if (got < static_cast<int>(kV1Size)) {
    return false;
  }
  uint32_t magic = 0;
  uint8_t version = record[kVersionAt];
  memcpy(&magic, record, sizeof(magic));
  memcpy(&basisPoints, record + kBasisAt, sizeof(basisPoints));
  if (version == EPUB_PERCENT_CACHE_VERSION && got == static_cast<int>(sizeof(record))) {
    memcpy(&mark.bp, record + kV1Size, sizeof(mark.bp));
    mark.serverAhead = (record[kV1Size + sizeof(mark.bp)] & SYNC_FLAG_SERVER_AHEAD) != 0;
  }
  if (mark.bp > 10000) mark = SyncMark{};
  return magic == EPUB_PERCENT_CACHE_MAGIC && (version == 1 || version == EPUB_PERCENT_CACHE_VERSION) &&
         basisPoints <= 10000;
}

// Same bytes tryWritePod() produced, written over the existing file: a torn
// write only costs a recompute, since the reader validates magic, version and range.
// hold: a reader exit; the write waits for the next flush point (HeldWrites).
void writeCachedEpubRecord(const std::string& cachePath, const uint16_t basisPoints, const SyncMark& mark,
                           const bool hold) {
  const uint8_t flags = mark.serverAhead ? SYNC_FLAG_SERVER_AHEAD : 0;
  uint8_t record[sizeof(EPUB_PERCENT_CACHE_MAGIC) + sizeof(EPUB_PERCENT_CACHE_VERSION) + sizeof(basisPoints) +
                 sizeof(mark.bp) + sizeof(flags)];
  uint8_t* out = record;
  memcpy(out, &EPUB_PERCENT_CACHE_MAGIC, sizeof(EPUB_PERCENT_CACHE_MAGIC));
  out += sizeof(EPUB_PERCENT_CACHE_MAGIC);
  memcpy(out, &EPUB_PERCENT_CACHE_VERSION, sizeof(EPUB_PERCENT_CACHE_VERSION));
  out += sizeof(EPUB_PERCENT_CACHE_VERSION);
  memcpy(out, &basisPoints, sizeof(basisPoints));
  out += sizeof(basisPoints);
  memcpy(out, &mark.bp, sizeof(mark.bp));
  out += sizeof(mark.bp);
  memcpy(out, &flags, sizeof(flags));
  if (!HeldWrites::saveFile(epubPercentCachePath(cachePath).c_str(), record, sizeof(record), hold)) {
    LOG_ERR("RBPR", "failed to write EPUB percent cache: %s", cachePath.c_str());
  }
}

uint16_t toBasisPoints(const float progress) {
  return static_cast<uint16_t>((clampProgressPercent(progress) * 100.0f) + 0.5f);
}

// KOSync marks held in RAM until a flush point (deep sleep and the others in
// the header), so a sync costs no card write of its own.
struct HeldMark {
  std::string cachePath;  // empty: free slot
  SyncMark mark;
};
constexpr size_t HELD_MARKS = 4;
std::mutex heldMutex;  // the KOSync task notes, Home and Library read on the render task
HeldMark heldMarks[HELD_MARKS];

HeldMark* findHeld(const std::string& cachePath) {
  for (auto& held : heldMarks) {
    if (held.cachePath == cachePath) return &held;
  }
  return nullptr;
}

void saveHeldMark(const HeldMark& held) {
  uint16_t basisPoints = 0;
  SyncMark saved;
  if (!readCachedEpubRecord(held.cachePath, basisPoints, saved)) {
    basisPoints = held.mark.bp;  // no % cache yet: the book's % at the check
  } else if (saved.bp == held.mark.bp && saved.serverAhead == held.mark.serverAhead) {
    return;
  }
  writeCachedEpubRecord(held.cachePath, basisPoints, held.mark, false);
}

float loadCachedEpubPercentFromCachePath(const std::string& cachePath, SyncMark& mark) {
  uint16_t basisPoints = 0;
  if (!readCachedEpubRecord(cachePath, basisPoints, mark)) {
    return -1.0f;
  }
  return static_cast<float>(basisPoints) / 100.0f;
}

void saveCachedEpubPercentToCachePath(const std::string& cachePath, const float progress, const bool hold) {
  if (cachePath.empty() || progress < 0.0f) {
    return;
  }

  const uint16_t basisPoints = toBasisPoints(progress);

  // The reader saves this on every exit; a read is cheaper on the card than
  // rewriting an identical value. The KOSync mark rides along unchanged.
  uint16_t existing = 0;
  SyncMark mark;
  if (readCachedEpubRecord(cachePath, existing, mark) && existing == basisPoints) {
    return;
  }
  writeCachedEpubRecord(cachePath, basisPoints, mark, hold);
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
  // Computed in memory only: browsing never writes the card. The reader saves the cache on exit.
  return progressPercent;
}

float loadEpubProgressPercent(const RecentBook& book) {
  Epub epub(book.path, "/.crossdink");
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
  return progressPercent;
}

float loadXtcProgressPercent(const RecentBook& book) {
  Xtc xtc(book.path, "/.crossdink");
  if (!xtc.load()) {
    return -1.0f;
  }

  uint8_t data[4];
  if (HeldWrites::readFile("RBPR", xtc.getCachePath() + "/progress.bin", data, sizeof(data)) != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return clampProgressPercent(static_cast<float>(xtc.calculateProgress(currentPage)));
}

float loadTxtProgressPercent(const RecentBook& book) {
  Txt txt(book.path, "/.crossdink");
  if (!txt.load()) {
    return -1.0f;
  }

  // TXT progress starts with a 16-bit page number; the remaining bytes are a
  // file offset used only to restore the reader page.
  // Read the reader's four-byte minimum so an interrupted write is rejected,
  // then decode only its two-byte page field.
  uint8_t progressData[4];
  if (HeldWrites::readFile("RBPR", txt.getCachePath() + "/progress.bin", progressData, sizeof(progressData)) !=
      static_cast<int>(sizeof(progressData))) {
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

float RecentBookProgress::loadCachedEpubPercent(const RecentBook& book, SyncMark* mark) {
  if (mark) *mark = SyncMark{};
  if (!FsHelpers::hasEpubExtension(book.path)) {
    return -1.0f;
  }
  return loadCachedEpubPercentAt(Epub::resolveCachePathForFilePath(book.path, "/.crossdink"), mark);
}

float RecentBookProgress::loadCachedEpubPercentAt(const std::string& cachePath, SyncMark* mark) {
  SyncMark saved;
  const float cachedProgress = loadCachedEpubPercentFromCachePath(cachePath, saved);
  if (mark) *mark = saved;
  if (cachedProgress >= 0.0f) {
    return cachedProgress;
  }
  return loadEpubSizeProgressPercentFromCachePath(cachePath);
}

float RecentBookProgress::loadSavedEpubPercent(const std::string& cachePath, SyncMark& mark) {
  return loadCachedEpubPercentFromCachePath(cachePath, mark);
}

void RecentBookProgress::saveCachedEpubPercent(const std::string& cachePath, const float progress) {
  saveCachedEpubPercentToCachePath(cachePath, progress, false);
}

void RecentBookProgress::saveCachedEpubPercent(const Epub& epub, const int spineIndex, const int currentPage,
                                               const int pageCount, const bool hold) {
  if (pageCount <= 0) {
    return;
  }
  saveCachedEpubPercentToCachePath(
      epub.getCachePath(), static_cast<float>(basisPoints(epub, spineIndex, currentPage, pageCount)) / 100.0f, hold);
}

uint16_t RecentBookProgress::basisPoints(const Epub& epub, const int spineIndex, const int currentPage,
                                         const int pageCount) {
  const float chapterProgress = static_cast<float>(currentPage + 1) / static_cast<float>(std::max(1, pageCount));
  return toBasisPoints(epub.calculateProgress(spineIndex, chapterProgress) * 100.0f);
}

void RecentBookProgress::noteSynced(const std::string& cachePath, const uint16_t bp, const bool serverAhead) {
  if (cachePath.empty()) return;
  HeldMark evicted;
  {
    std::lock_guard<std::mutex> lock(heldMutex);
    HeldMark* slot = findHeld(cachePath);
    if (!slot) slot = findHeld(std::string());
    if (!slot) {
      // Five books checked between flush points: the oldest note is written now.
      evicted = std::move(heldMarks[0]);
      std::move(heldMarks + 1, heldMarks + HELD_MARKS, heldMarks);
      slot = &heldMarks[HELD_MARKS - 1];
    }
    slot->cachePath = cachePath;
    slot->mark = SyncMark{bp, serverAhead};
  }
  LOG_INF("RBPR", "KOSync mark %s: %u bp%s (held)", cachePath.c_str(), static_cast<unsigned>(bp),
          serverAhead ? ", server ahead" : "");
  if (!evicted.cachePath.empty()) saveHeldMark(evicted);
}

void RecentBookProgress::flushSyncMarks() {
  HeldMark pending[HELD_MARKS];
  {
    std::lock_guard<std::mutex> lock(heldMutex);
    std::move(heldMarks, heldMarks + HELD_MARKS, pending);
    for (auto& held : heldMarks) held.cachePath.clear();
  }
  for (const auto& held : pending) {
    if (!held.cachePath.empty()) saveHeldMark(held);
  }
}

RecentBookProgress::SyncMark RecentBookProgress::syncMark(const std::string& cachePath) {
  {
    std::lock_guard<std::mutex> lock(heldMutex);
    if (const HeldMark* held = findHeld(cachePath)) return held->mark;
  }
  SyncMark mark;
  uint16_t basisPoints = 0;
  if (!readCachedEpubRecord(cachePath, basisPoints, mark)) return {};
  return mark;
}

RecentBookProgress::SyncBadge RecentBookProgress::syncBadge(const std::string& cachePath, const float progress,
                                                            SyncMark mark) {
  if (cachePath.empty() || !hasPercent(progress) || !KOREADER_STORE.hasCredentials()) return SyncBadge::None;
  if (kosync_auto::pushPendingFor(cachePath)) return SyncBadge::Syncing;
  {
    std::lock_guard<std::mutex> lock(heldMutex);
    if (const HeldMark* held = findHeld(cachePath)) mark = held->mark;
  }
  if (mark.bp == SyncMark::kNever) return SyncBadge::None;
  if (mark.serverAhead) return SyncBadge::ServerAhead;
  return toBasisPoints(progress) > mark.bp ? SyncBadge::Unsynced : SyncBadge::Synced;
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
