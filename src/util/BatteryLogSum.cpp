#include "BatteryLogSum.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <HalStorage.h>
#include <Knobs.h>
#include <Logging.h>
#include <esp_rom_crc.h>

#include <algorithm>

#include "BatteryLog.h"

namespace BatteryLogSum {
namespace {
constexpr char SUM_PATH[] = "/debug/logs/battery.sum";
constexpr char SUM_TMP_PATH[] = "/debug/logs/battery.sum.tmp";
constexpr uint32_t SUM_MAGIC = 0x42535541;  // "BSUA": bump on any parse rule change
struct Header {
  uint32_t magic;
  uint32_t size;     // sizeof(BatteryLogParser): a layout change drops the file
  uint32_t offset;   // after the last full row read
  uint32_t tailCrc;  // of the up to 64 bytes before offset
  uint32_t crc;      // of the parser that follows
};

// CRC of the up to 64 bytes before offset; 0 when they can't be read.
uint32_t tailCrc(HalFile& f, const uint32_t offset) {
  uint8_t b[64];
  const uint32_t n = std::min<uint32_t>(offset, sizeof(b));
  if (n == 0 || !f.seekSet(offset - n) || f.read(b, n) != static_cast<int>(n)) return 0;
  return esp_rom_crc32_le(0, b, n);
}

uint32_t parserCrc(const BatteryLogParser& p) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&p), sizeof(p));
}
}  // namespace

bool load(BatteryLogParser& p, int& fileIndex, uint32_t& offset) {
  HalFile f = Storage.open(SUM_PATH, O_RDONLY);
  if (!f) return false;
  Header h{};
  const bool ok = f.read(&h, sizeof(h)) == static_cast<int>(sizeof(h)) && h.magic == SUM_MAGIC && h.size == sizeof(p) &&
                  f.read(&p, sizeof(p)) == static_cast<int>(sizeof(p)) && parserCrc(p) == h.crc &&
                  p.stateSkipS == KNOBS.batteryStateSkipS && p.halfLifeH == KNOBS.batteryHalfLifeH;
  f.close();
  if (!ok) {
    LOG_INF("BAT", "battery.sum unusable");
    return false;
  }
  for (int i = 0; i < BatteryLog::LOG_FILES; ++i) {
    HalFile log = Storage.open(BatteryLog::LOG_PATHS[i], O_RDONLY);
    const bool match = log && log.fileSize() >= h.offset && tailCrc(log, h.offset) == h.tailCrc;
    log.close();
    if (match) {
      fileIndex = i;
      offset = h.offset;
      return true;
    }
  }
  LOG_INF("BAT", "battery.sum matches no log file");
  return false;
}

void save(const BatteryLogParser& p, const int fileIndex, const uint32_t offset) {
  HalFile src = Storage.open(BatteryLog::LOG_PATHS[fileIndex], O_RDONLY);
  const Header h{SUM_MAGIC, sizeof(p), offset, src ? tailCrc(src, offset) : 0, parserCrc(p)};
  src.close();
  if (h.tailCrc == 0) return;
  HalFile f = Storage.open(SUM_TMP_PATH, O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = f && f.write(&h, sizeof(h)) == sizeof(h) && f.write(&p, sizeof(p)) == sizeof(p);
  ok = f.close() && ok;
  // Removed first: rename does not replace. A failure in between only costs a full read.
  ok = ok && (!Storage.exists(SUM_PATH) || Storage.remove(SUM_PATH)) && Storage.rename(SUM_TMP_PATH, SUM_PATH);
  if (!ok) LOG_ERR("BAT", "Failed to save %s", SUM_PATH);
}
}  // namespace BatteryLogSum

#endif  // CROSSDINK_GOODIES && !SIMULATOR
