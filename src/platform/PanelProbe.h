#pragma once

#if !defined(SIMULATOR) && !FREEINK_MCU_C3
#include <XteinkDetect.h>

// S3 panel-controller resolution, probed once per unit. The display-bus probe
// (XteinkDetect) costs 120-145 ms. Its decisive result is kept in NVS with the
// chip's eFuse MAC, and every later boot restores it instead of probing. A
// flash erase or an image copied from another unit (MAC mismatch) probes again.
// shortcut: a swapped panel keeps the old record; erase NVS (full flash) after a panel swap.
// EpdBus::begin() releases the sleep hold on RESET, so skipping the probe's
// release is safe.
namespace PanelProbe {
// Before display.begin(): sets BoardConfig::ACTIVE's controller and variant.
void resolve();
// This boot's probe diagnostics: the live probe's, or the recorded one's.
const freeink::XteinkDisplayProbeDiag& diag();
// True when resolve() restored the NVS record instead of probing.
bool cached();
}  // namespace PanelProbe
#endif
