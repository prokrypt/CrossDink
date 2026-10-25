#pragma once

#if !defined(SIMULATOR) && !FREEINK_MCU_C3
#include <XteinkDetect.h>

// S3 panel-controller resolution with a deep-sleep shortcut. The display-bus
// probe (XteinkDetect) costs 120-145 ms of every boot. Its decisive result is
// kept in RTC memory (CRC-checked), and a wake from deep sleep restores it
// instead of probing: the panel cannot change while RTC memory stays powered.
// Any other boot (power-on, restart, crash, OTA) probes and records again.
// EpdBus::begin() releases the sleep hold on RESET, so skipping the probe's
// release is safe.
namespace PanelProbe {
// Before display.begin(): sets BoardConfig::ACTIVE's controller and variant.
void resolve();
// This boot's probe diagnostics: the live probe's, or the recorded one's.
const freeink::XteinkDisplayProbeDiag& diag();
// True when resolve() restored the record instead of probing.
bool cached();
}  // namespace PanelProbe
#endif
