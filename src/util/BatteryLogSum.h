#pragma once

#include <cstdint>

#include "BatteryLogParser.h"

// /debug/logs/battery.sum: a BatteryLogParser after the last full row of a
// battery log file, and where that row ends. The CRC of the bytes before that
// offset finds the file again after rotations renamed it. Goodies builds (X4 Pro).
namespace BatteryLogSum {
// Fills p and the LOG_PATHS index + offset to read on from; false (p undefined)
// when the file is missing, bad, from another parser layout or matches no log file.
bool load(BatteryLogParser& p, int& fileIndex, uint32_t& offset);
// p as it was right after the row ending at offset in LOG_PATHS[fileIndex].
void save(const BatteryLogParser& p, int fileIndex, uint32_t offset);
}  // namespace BatteryLogSum
