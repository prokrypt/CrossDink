#pragma once

#include <cstddef>

// Read-only eFuse security state, read once at boot (a few bytes cached).
// "Locked" means the unit can no longer be reflashed or debugged freely: flash
// encryption in release mode, secure boot, download mode off, or the
// USB-Serial-JTAG disabled. Nothing here burns eFuses.
namespace DeviceSecurity {
struct State {
  bool read = false;             // false in the simulator / non-S3 builds
  const char* flashEnc = "off";  // off | dev | release
  bool secureBoot = false;
  bool usbSerialJtagDisabled = false;
  bool jtagDisabled = false;    // USB JTAG, pad JTAG or soft-disabled
  const char* download = "on";  // on | secure | off
  bool locked = false;
};

// Reads the eFuses on first call; cheap afterwards.
const State& get();
// "fenc=off sboot=0 usbjtag=on jtag=on dl=on locked=0"
size_t format(char* buf, size_t size);
}  // namespace DeviceSecurity
