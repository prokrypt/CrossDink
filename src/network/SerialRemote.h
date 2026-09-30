#pragma once

#include <cstddef>
#include <cstdint>

// Serial remote control for test automation (debug builds, CROSSDINK_SERIAL_REMOTE).
// Line commands arrive on the USB serial "CMD:" channel owned by
// UsbSerialFileTransfer; replies are single lines "OK:<VERB> ..." or
// "ERR:<VERB>:<reason>". See docs/serial-remote.md for the command list.
namespace SerialRemote {

// True when a normalized SD path names /debug/remote-token (any build), or with
// orFolder also /debug itself. The web server and WebDAV refuse these so the
// token is never served, copied or moved out, or replaced over the network.
bool isTokenPath(const char* path, bool orFolder = false);

#if CROSSDINK_SERIAL_REMOTE
// Handles one received line (without the newline). False when the line is not
// a remote-control command, so the caller can try its own commands.
bool handleLine(const char* line);
// Advances timed input injection, queued typing and pending WAITIDLE replies.
// Main task, once per loop.
void poll();
// Wi-Fi remote (POST /api/cmd): hands one command ("KBDEXP 15 6", no "CMD:")
// from another task to the main task and waits for its reply line. The token
// must match /debug/remote-token on the SD card (no file = disabled). Returns
// an HTTP status: 200, 403 bad token, 404 unknown command, 503 busy/timeout.
int runFromOtherTask(const char* token, const char* cmd, char* out, size_t outLen, uint32_t timeoutMs);
// After runFromOtherTask(..., "SCREENSHOT", ...) returned 200: the PBM image the
// main task captured. Server task only; valid until the next SCREENSHOT.
const uint8_t* screenshot(size_t& len);
#else
inline bool handleLine(const char*) { return false; }
inline void poll() {}
#endif

}  // namespace SerialRemote
