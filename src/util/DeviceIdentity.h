#pragma once

#include <cstddef>

// Debug-log identity of this unit: which panel controller it carries and how
// that was decided, plus a one-shot header for log grabs (the PSRAM ring can
// wrap past the boot lines). Release builds compile the bodies out.
namespace DeviceIdentity {
// Exact controller silicon ("UC8179", "SSD1677", ...).
const char* controllerName();
// Boot: controller, detect method, VER/FLG bytes, MTP product id and LUT
// version, and the raw MTP block when the probe captured it (LOG_DBG).
void logPanel();
// "=== header ===" line(s) for the top of a log dump: device, serial, build
// sha and env, controller and panel id, uptime and heap. Returns the length
// written (0 in release builds).
size_t formatLogHeader(char* buf, size_t size);
}  // namespace DeviceIdentity
