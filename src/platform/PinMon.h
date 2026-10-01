#pragma once

#include <cstddef>

// Debug (CROSSDINK_SERIAL_REMOTE): passive monitor of the X4 Pro's unused GPIOs
// (15-17, 45-48), to find what, if anything, is wired to them. Each level change
// is logged to the PSRAM ring as "[PIN] PINMON <pin> rise|fall t=<ms>" ("PINMON
// wake pin <pin> ..." when it ended a light sleep), plus a per-minute summary.
// The pins are inputs only, never driven, and are light-sleep wake sources. A pin
// with more than 20 changes in a minute is floating or chattering: it is released
// until reboot so it cannot keep the device awake.
namespace PinMon {
#if CROSSDINK_SERIAL_REMOTE && defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)
// Main task, once after InputWake::begin(): starts the monitor (on by default).
void begin();
// Any task: switches the monitor; its task applies it.
void setEnabled(bool on);
bool enabled();
// "on|off 15:<level>/<changes>/<wakes> ... chatter=<pins>".
void status(char* out, size_t len);
#else
inline void begin() {}
inline bool enabled() { return false; }
#endif
}  // namespace PinMon
