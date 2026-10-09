#pragma once

#include <cstdint>

// Screen burn-in guard. One random seed per boot (a deep-sleep wake is a
// boot) picks the dither phase and the small offset that static chrome
// (headers, top status bar) is drawn at. Both stay put for the whole boot, so
// they only change what refreshes that already happen draw.
namespace AntiBurnIn {
uint32_t seed();
// Chrome offset, each in [-KNOBS.burnInShiftPx, KNOBS.burnInShiftPx].
int shiftX();
int shiftY();
}  // namespace AntiBurnIn
