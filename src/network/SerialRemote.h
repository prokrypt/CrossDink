#pragma once

// Serial remote control for test automation (debug builds, CROSSDINK_SERIAL_REMOTE).
// Line commands arrive on the USB serial "CMD:" channel owned by
// UsbSerialFileTransfer; replies are single lines "OK:<VERB> ..." or
// "ERR:<VERB>:<reason>". See docs/serial-remote.md for the command list.
namespace SerialRemote {

#if CROSSDINK_SERIAL_REMOTE
// Handles one received line (without the newline). False when the line is not
// a remote-control command, so the caller can try its own commands.
bool handleLine(const char* line);
// Advances timed input injection, queued typing and pending WAITIDLE replies.
// Main task, once per loop.
void poll();
#else
inline bool handleLine(const char*) { return false; }
inline void poll() {}
#endif

}  // namespace SerialRemote
