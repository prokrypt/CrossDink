#include "EpubLinkReturnState.h"

#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstdint>

#include "HeldWrites.h"

namespace EpubLinkReturnState {
std::optional<ReadingProgress> readingProgress(const Position (&positions)[MAX_DEPTH], const int depth,
                                               const bool transientPreview, const ReaderProgressSaveDebouncer& progress,
                                               const int lastSavedSpineIndex, const int lastSavedPageCount) {
  if (transientPreview && depth > 0) {
    const auto& origin = positions[resumeDepth(depth, true)];
    const uint32_t originKey =
        (static_cast<uint32_t>(origin.spineIndex) << 16) | static_cast<uint16_t>(origin.pageNumber);
    const int pageCount = progress.lastObservedPosition() == originKey
                              ? static_cast<int>(progress.lastObservedMetadata())
                              : (lastSavedSpineIndex == origin.spineIndex ? std::max(0, lastSavedPageCount) : 0);
    return ReadingProgress{origin.spineIndex, origin.pageNumber, pageCount};
  }
  if (!progress.hasPending()) return std::nullopt;
  const uint32_t position = progress.lastObservedPosition();
  return ReadingProgress{static_cast<int>(position >> 16), static_cast<int>(position & 0xffffU),
                         static_cast<int>(progress.lastObservedMetadata())};
}

void push(Position (&positions)[MAX_DEPTH], int& depth, const Position origin) {
  if (depth == MAX_DEPTH) {
    for (int i = 1; i < MAX_DEPTH; ++i) positions[i - 1] = positions[i];
    --depth;
  }
  positions[depth++] = origin;
}

int resumeDepth(const int depth, const bool transientPreview) {
  return transientPreview && depth > 0 ? depth - 1 : depth;
}

namespace {
// Fixed size so a rewrite stays in place; unused pairs are zero.
constexpr size_t RECORD_BYTES = 1 + MAX_DEPTH * 4;

bool present(const std::string& path) {
  uint8_t none;
  return HeldWrites::heldFile(path.c_str(), &none, 0) >= 0 || Storage.exists(path.c_str());
}

// Held until the next flush point (sleep, restart); a reset carries it in PSRAM.
bool write(const std::string& path, const uint8_t (&data)[RECORD_BYTES]) {
  if (HeldWrites::saveFile(path.c_str(), data, RECORD_BYTES, /*hold=*/true)) return true;
  LOG_ERR("ERS", "Failed to write link return stack");
  return false;
}
}  // namespace

bool save(const std::string& cachePath, const Position (&positions)[MAX_DEPTH], const int depth,
          const bool transientPreview) {
  const std::string path = cachePath + "/links.bin";
  const int savedDepth = resumeDepth(depth, transientPreview);
  if (savedDepth < 0 || savedDepth > MAX_DEPTH) return false;
  // Books never linked from get no file; empty history only clears an existing record.
  if (savedDepth == 0 && !present(path)) return true;

  uint8_t data[RECORD_BYTES] = {};
  data[0] = static_cast<uint8_t>(savedDepth);
  for (int i = 0; i < savedDepth; ++i) {
    const auto& pos = positions[i];
    if (pos.spineIndex < 0 || pos.spineIndex > UINT16_MAX || pos.pageNumber < 0 || pos.pageNumber > UINT16_MAX) {
      LOG_ERR("ERS", "Link return position out of range");
      return false;
    }
    uint8_t* p = data + 1 + i * 4;
    p[0] = pos.spineIndex & 0xff;
    p[1] = (pos.spineIndex >> 8) & 0xff;
    p[2] = pos.pageNumber & 0xff;
    p[3] = (pos.pageNumber >> 8) & 0xff;
  }
  return write(path, data);
}

bool load(const std::string& cachePath, Position (&positions)[MAX_DEPTH], int& depth, const int spineCount) {
  depth = 0;
  const std::string path = cachePath + "/links.bin";
  if (!present(path)) return true;
  uint8_t data[RECORD_BYTES + 1];  // one spare byte detects an oversized file
  const int size = HeldWrites::readFile("ERS", path, data, sizeof(data));
  if (size == static_cast<int>(RECORD_BYTES) && data[0] == 0) return true;  // already consumed
  const int loadedDepth = size > 0 ? data[0] : 0;
  bool valid = size == static_cast<int>(RECORD_BYTES) && loadedDepth >= 1 && loadedDepth <= MAX_DEPTH;
  for (int i = 0; valid && i < loadedDepth; ++i) {
    const uint8_t* p = data + 1 + i * 4;
    valid = (p[0] | (p[1] << 8)) < spineCount;
  }
  // Consume even malformed records. A later clean exit rewrites the current
  // stack; a crash must not resurrect old entries.
  static constexpr uint8_t EMPTY[RECORD_BYTES] = {};
  if (!write(path, EMPTY)) return false;
  if (!valid) {
    LOG_ERR("ERS", "Ignoring invalid link return stack");
    return false;
  }
  for (int i = 0; i < loadedDepth; ++i) {
    const uint8_t* p = data + 1 + i * 4;
    positions[i] = {p[0] | (p[1] << 8), p[2] | (p[3] << 8)};
  }
  depth = loadedDepth;
  LOG_DBG("ERS", "Loaded link stack, depth %d", depth);
  return true;
}
}  // namespace EpubLinkReturnState
