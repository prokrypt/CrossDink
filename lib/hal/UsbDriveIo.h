#pragma once

#include <cstdint>

// Host I/O totals while USB Drive is up; stamped on the USB task, read on the
// main loop (drives the transfer light).
struct UsbDriveIo {
  uint32_t firstIoMs = 0;  // millis() of the first host read/write, 0 = none yet
  uint32_t lastIoMs = 0;   // millis() of the last host read/write, 0 = none yet
  uint32_t ops = 0;        // host read/write calls
  uint32_t readBytes = 0;
  uint32_t writeBytes = 0;
};
