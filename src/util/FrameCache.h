#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Finished screen frames (the B/W frame, or both Direct gray planes) on SD, so
// a repeat of the same sleep or boot image skips the BMP decode.

// Cache file for sourcePath. variant names everything else that changes the
// output (mode, orientation, screen size); file identity and the firmware
// version (decoder changes) are added here.
std::string frameCachePath(const std::string& sourcePath, FsFile& file, const char* variant);
bool readFrameCache(const std::string& path, uint8_t* const* buffers, int count, size_t bytes);
void writeFrameCache(const std::string& path, const uint8_t* const* buffers, int count, size_t bytes);
