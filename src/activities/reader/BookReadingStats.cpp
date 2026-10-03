#include "BookReadingStats.h"

#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>

#include "ReaderExitSave.h"
#include "TwoSlotFile.h"

namespace {
// Binary layout v1 (11 bytes):
//   [0]     version (= 1)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//
// Binary layout v2 (12 bytes):
//   [0]     version (= 2)
//   [1-2]   sessionCount        uint16_t LE
//   [3-6]   totalReadingSeconds uint32_t LE
//   [7-10]  totalPagesTurned    uint32_t LE
//   [11]    isCompleted         uint8_t
//
// Binary layout v3 (16 bytes):
//   [0]      version (= 3)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//
// Binary layout v4 (69 bytes):
//   [0]      version (= 4)
//   [1-2]    sessionCount              uint16_t LE
//   [3-6]    totalReadingSeconds       uint32_t LE
//   [7-10]   totalPagesTurned          uint32_t LE
//   [11]     isCompleted               uint8_t
//   [12-13]  avgSecondsPerForwardPage  uint16_t LE
//   [14-15]  paceSampleCount           uint16_t LE
//   [16]     flags bit0=startDateManual bit1=finishedDateManual
//   [17-18]  startDate.year            uint16_t LE
//   [19]     startDate.month           uint8_t
//   [20]     startDate.day             uint8_t
//   [21-22]  finishedDate.year         uint16_t LE
//   [23]     finishedDate.month        uint8_t
//   [24]     finishedDate.day          uint8_t
//   [25-40]  timeOfDaySeconds[4]       uint32_t LE each
//   [41-68]  dayOfWeekSeconds[7]       uint32_t LE each
//
// Binary layout v5 (73 bytes):
//   [0-68]   v4 fields
//   [69-72]  estimatedTimeLeftSeconds  uint32_t LE, 0 means unavailable
static constexpr uint8_t STATS_FILE_VERSION = 5;
static constexpr uint8_t STATS_FILE_VERSION_V2 = 2;
static constexpr uint8_t STATS_FILE_VERSION_V1 = 1;
static constexpr uint8_t STATS_FILE_VERSION_V3 = 3;
static constexpr uint8_t STATS_FILE_VERSION_V4 = 4;
static constexpr int STATS_FILE_SIZE_V1 = 11;
static constexpr int STATS_FILE_SIZE_V2 = 12;
static constexpr int STATS_FILE_SIZE_V3 = 16;
static constexpr int STATS_FILE_SIZE_V4 = 69;
static constexpr int STATS_FILE_SIZE = 73;
static_assert(STATS_FILE_SIZE <= two_slot::MAX_PAYLOAD_BYTES, "stats slot exceeds two_slot buffers");
static constexpr uint16_t MAX_PACE_SAMPLE_COUNT = 1000;
static constexpr uint8_t FLAG_START_DATE_MANUAL = 1u << 0;
static constexpr uint8_t FLAG_FINISHED_DATE_MANUAL = 1u << 1;
static constexpr uint8_t PREVIOUS_VERSIONED_STATS_FILE_VERSION = STATS_FILE_VERSION - 1;
static constexpr const char* LEGACY_STATS_FILE_NAME = "stats.bin";

std::string statsFileNameForVersion(const uint8_t version) {
  char buf[16];
  snprintf(buf, sizeof(buf), "stats_v%u.bin", version);
  return std::string(buf);
}

// Formats older firmware wrote before the two slots; read once, replaced by the first save.
bool openMigrationSource(const std::string& cachePath, FsFile& f) {
  // When bumping STATS_FILE_VERSION, this automatically tries the previous
  // versioned filename (e.g. v6 falls back to stats_v5.bin) before the original
  // unversioned stats.bin migration source.
  // cppcheck-suppress unreadVariable ; read only by LOG_DBG, compiled out in release
  const std::string currentName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string previousName = statsFileNameForVersion(PREVIOUS_VERSIONED_STATS_FILE_VERSION);
  const std::string previousPath = cachePath + "/" + previousName;
  if (Storage.exists(previousPath.c_str()) && Storage.openFileForRead("STATS", previousPath, f)) {
    LOG_DBG("STATS", "Migrating %s to %s", previousName.c_str(), currentName.c_str());
    return true;
  }

  const std::string legacyPath = cachePath + "/" + LEGACY_STATS_FILE_NAME;
  if (Storage.exists(legacyPath.c_str()) && Storage.openFileForRead("STATS", legacyPath, f)) {
    LOG_DBG("STATS", "Migrating legacy %s to %s", LEGACY_STATS_FILE_NAME, currentName.c_str());
    return true;
  }

  return false;
}

// Every layout load() understands; anything else falls through to the other slot.
bool isKnownLayout(const uint8_t* data, const size_t size) {
  const size_t n = std::min(size, static_cast<size_t>(STATS_FILE_SIZE));
  return (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) ||
         (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) ||
         (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) ||
         (n == STATS_FILE_SIZE_V4 && data[0] == STATS_FILE_VERSION_V4) ||
         (n == STATS_FILE_SIZE && data[0] == STATS_FILE_VERSION);
}

uint16_t readLe16(const uint8_t* data, const int offset) {
  return static_cast<uint16_t>(data[offset]) | (static_cast<uint16_t>(data[offset + 1]) << 8);
}

uint32_t readLe32(const uint8_t* data, const int offset) {
  return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
}

void readCommonStats(const uint8_t* data, BookReadingStats& stats) {
  stats.sessionCount = readLe16(data, 1);
  stats.totalReadingSeconds = readLe32(data, 3);
  stats.totalPagesTurned = readLe32(data, 7);
}

void writeLe16(uint8_t* data, const int offset, const uint16_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
}

void writeLe32(uint8_t* data, const int offset, const uint32_t value) {
  data[offset] = value & 0xFF;
  data[offset + 1] = (value >> 8) & 0xFF;
  data[offset + 2] = (value >> 16) & 0xFF;
  data[offset + 3] = (value >> 24) & 0xFF;
}

ReadingStatsDate readDate(const uint8_t* data, const int offset) {
  ReadingStatsDate date;
  date.year = readLe16(data, offset);
  date.month = data[offset + 2];
  date.day = data[offset + 3];
  if (!date.isValid()) {
    date.clear();
  }
  return date;
}

}  // namespace

BookReadingStats BookReadingStats::load(const std::string& cachePath) {
  if (const BookReadingStats* held = ReaderExitSave::book(cachePath)) return *held;  // newer than the file
  BookReadingStats stats;
  uint8_t data[two_slot::MAX_PAYLOAD_BYTES] = {};
  const std::string statsPath = cachePath + "/" + statsFileNameForVersion(STATS_FILE_VERSION);
  int n = static_cast<int>(std::min(
      two_slot::read("STATS", statsPath.c_str(), (statsPath + ".bak").c_str(), STATS_FILE_SIZE, isKnownLayout, data),
      static_cast<size_t>(STATS_FILE_SIZE)));
  if (n == 0) {
    FsFile f;
    if (!openMigrationSource(cachePath, f)) return stats;
    n = f.read(data, STATS_FILE_SIZE);
    f.close();
  }

  if (n == STATS_FILE_SIZE_V1 && data[0] == STATS_FILE_VERSION_V1) {
    readCommonStats(data, stats);
    return stats;
  }

  if (n == STATS_FILE_SIZE_V2 && data[0] == STATS_FILE_VERSION_V2) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    return stats;
  }

  if (n == STATS_FILE_SIZE_V3 && data[0] == STATS_FILE_VERSION_V3) {
    readCommonStats(data, stats);
    stats.isCompleted = data[11] != 0;
    stats.avgSecondsPerForwardPage = readLe16(data, 12);
    stats.paceSampleCount = readLe16(data, 14);
    return stats;
  }

  if (n != STATS_FILE_SIZE && n != STATS_FILE_SIZE_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  if (n == STATS_FILE_SIZE_V4 && data[0] != STATS_FILE_VERSION_V4) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  if (n == STATS_FILE_SIZE && data[0] != STATS_FILE_VERSION) {
    LOG_DBG("STATS", "Stats missing or version mismatch, starting fresh");
    return stats;
  }
  readCommonStats(data, stats);
  stats.isCompleted = data[11] != 0;
  stats.avgSecondsPerForwardPage = readLe16(data, 12);
  stats.paceSampleCount = readLe16(data, 14);
  const uint8_t flags = data[16];
  stats.startDateManual = (flags & FLAG_START_DATE_MANUAL) != 0;
  stats.finishedDateManual = (flags & FLAG_FINISHED_DATE_MANUAL) != 0;
  stats.startDate = readDate(data, 17);
  stats.finishedDate = readDate(data, 21);
  for (size_t i = 0; i < stats.timeOfDaySeconds.size(); ++i) {
    stats.timeOfDaySeconds[i] = readLe32(data, 25 + static_cast<int>(i) * 4);
  }
  for (size_t i = 0; i < stats.dayOfWeekSeconds.size(); ++i) {
    stats.dayOfWeekSeconds[i] = readLe32(data, 41 + static_cast<int>(i) * 4);
  }
  if (n == STATS_FILE_SIZE) {
    stats.estimatedTimeLeftSeconds = readLe32(data, 69);
  }
  return stats;
}

void BookReadingStats::recordForwardPageRead(uint32_t seconds) {
  if (seconds == 0) {
    return;
  }
  if (seconds > UINT16_MAX) {
    seconds = UINT16_MAX;
  }

  const uint16_t sample = static_cast<uint16_t>(seconds);
  if (paceSampleCount == 0 || avgSecondsPerForwardPage == 0) {
    avgSecondsPerForwardPage = sample;
    paceSampleCount = 1;
    return;
  }

  const uint16_t weight = paceSampleCount < MAX_PACE_SAMPLE_COUNT ? paceSampleCount : MAX_PACE_SAMPLE_COUNT;
  const uint32_t nextAverage =
      (static_cast<uint32_t>(avgSecondsPerForwardPage) * weight + sample) / (static_cast<uint32_t>(weight) + 1U);
  avgSecondsPerForwardPage = static_cast<uint16_t>(nextAverage);
  if (paceSampleCount < MAX_PACE_SAMPLE_COUNT) {
    paceSampleCount++;
  }
}

void BookReadingStats::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  recordReadingSpanIntoBuckets(timeOfDaySeconds, dayOfWeekSeconds, localStart, seconds);
}

void BookReadingStats::formatDuration(uint32_t seconds, char* buf, size_t len) {
  if (seconds < 60) {
    snprintf(buf, len, "%s", tr(STR_STATS_LESS_THAN_MIN));
    return;
  }
  const uint32_t hours = seconds / 3600;
  const uint32_t minutes = (seconds % 3600) / 60;
  if (hours == 0) {
    snprintf(buf, len, "%lu min", static_cast<unsigned long>(minutes));
  } else {
    snprintf(buf, len, "%luh %lu min", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes));
  }
}

bool BookReadingStats::save(const std::string& cachePath) const {
  const std::string statsPath = cachePath + "/" + statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string backupPath = statsPath + ".bak";

  uint8_t data[STATS_FILE_SIZE];
  memset(data, 0, sizeof(data));
  data[0] = STATS_FILE_VERSION;
  writeLe16(data, 1, sessionCount);
  writeLe32(data, 3, totalReadingSeconds);
  writeLe32(data, 7, totalPagesTurned);
  data[11] = isCompleted ? 1 : 0;
  writeLe16(data, 12, avgSecondsPerForwardPage);
  writeLe16(data, 14, paceSampleCount);
  data[16] = (startDateManual ? FLAG_START_DATE_MANUAL : 0u) | (finishedDateManual ? FLAG_FINISHED_DATE_MANUAL : 0u);
  writeLe16(data, 17, startDate.isValid() ? startDate.year : 0);
  data[19] = startDate.isValid() ? startDate.month : 0;
  data[20] = startDate.isValid() ? startDate.day : 0;
  writeLe16(data, 21, finishedDate.isValid() ? finishedDate.year : 0);
  data[23] = finishedDate.isValid() ? finishedDate.month : 0;
  data[24] = finishedDate.isValid() ? finishedDate.day : 0;
  for (size_t i = 0; i < timeOfDaySeconds.size(); ++i) {
    writeLe32(data, 25 + static_cast<int>(i) * 4, timeOfDaySeconds[i]);
  }
  for (size_t i = 0; i < dayOfWeekSeconds.size(); ++i) {
    writeLe32(data, 41 + static_cast<int>(i) * 4, dayOfWeekSeconds[i]);
  }
  writeLe32(data, 69, estimatedTimeLeftSeconds);

  // Reader exit saves unconditionally; a quick open-and-close writes nothing.
  return two_slot::write("STATS", statsPath.c_str(), backupPath.c_str(), data, sizeof(data));
}

bool BookReadingStats::remove(const std::string& cachePath) {
  const std::string statsFileName = statsFileNameForVersion(STATS_FILE_VERSION);
  const std::string statsPath = cachePath + "/" + statsFileName;
  const std::string previousStatsFileName = statsFileNameForVersion(PREVIOUS_VERSIONED_STATS_FILE_VERSION);
  const std::string previousStatsPath = cachePath + "/" + previousStatsFileName;
  const std::string legacyStatsPath = cachePath + "/" + LEGACY_STATS_FILE_NAME;
  bool ok = true;

  // Remove fallback sources from oldest to newest. Keeping the canonical file
  // until every fallback is gone prevents an interrupted reset from reviving
  // older stats on the next load.
  if (Storage.exists(legacyStatsPath.c_str()) && !Storage.remove(legacyStatsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", LEGACY_STATS_FILE_NAME);
    ok = false;
  }
  if (Storage.exists(previousStatsPath.c_str()) && !Storage.remove(previousStatsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", previousStatsFileName.c_str());
    ok = false;
  }
  for (const char* suffix : {".tmp", ".bak"}) {
    const std::string transactionPath = statsPath + suffix;
    if (Storage.exists(transactionPath.c_str()) && !Storage.remove(transactionPath.c_str())) {
      LOG_ERR("STATS", "Could not delete %s", transactionPath.c_str());
      ok = false;
    }
  }
  // Keep the canonical file when a recovery artifact could not be removed.
  // Otherwise a later load could restore stats that the user just reset.
  if (ok && Storage.exists(statsPath.c_str()) && !Storage.remove(statsPath.c_str())) {
    LOG_ERR("STATS", "Could not delete %s", statsFileName.c_str());
    ok = false;
  }
  return ok;
}
