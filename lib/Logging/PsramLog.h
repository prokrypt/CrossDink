#pragma once

#include <cstddef>
#include <cstdint>

// Debug-only text log ring in PSRAM that survives software restarts (silent
// reboots, panics, OTA restarts), so the log leading up to a reboot can be
// fetched afterwards over Wi-Fi (GET /api/psram-log) or USB serial
// (CMD:PSRAMLOG). Power-on reset, the reset button and deep sleep power PSRAM
// down and start a fresh ring.
//
// Enabled with -DCROSSINK_PSRAM_LOG=1. Needs
// CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y ([dualpoint_cores] in
// platformio.ini); the boot PSRAM test skips that segment. Otherwise every
// function is an empty inline.
#if CROSSINK_PSRAM_LOG && !defined(SIMULATOR)
namespace PsramLog {
// Appends text; safe from any task on either core.
void append(const char* text, size_t len);
// Byte offsets of the oldest text still in the ring and of the end of the
// text written so far. Readers copy [oldest, end) so logging done while they
// stream does not keep extending the dump.
uint32_t oldest();
uint32_t end();
// Copies up to maxLen bytes starting at cursor into dst and advances cursor.
// A cursor the writer has lapped snaps forward to the oldest byte. Returns 0
// at the end of the log.
size_t read(uint32_t& cursor, char* dst, size_t maxLen);
}  // namespace PsramLog
#else
namespace PsramLog {
inline void append(const char*, size_t) {}
inline uint32_t oldest() { return 0; }
inline uint32_t end() { return 0; }
inline size_t read(uint32_t&, char*, size_t) { return 0; }
}  // namespace PsramLog
#endif
