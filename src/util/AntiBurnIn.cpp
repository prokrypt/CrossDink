#include "AntiBurnIn.h"

#include <Knobs.h>

#if defined(SIMULATOR)
#include <ctime>
#else
#include <esp_random.h>
#endif

namespace AntiBurnIn {

uint32_t seed() {
#if defined(SIMULATOR)
  static const uint32_t s = static_cast<uint32_t>(time(nullptr)) * 2654435761u;
#else
  static const uint32_t s = esp_random();
#endif
  return s;
}

namespace {
int pick(const uint32_t bits) {
  const int range = KNOBS.burnInShiftPx;
  return static_cast<int>(bits % static_cast<uint32_t>(2 * range + 1)) - range;
}
}  // namespace

int shiftX() { return pick(seed() >> 2); }
int shiftY() { return pick(seed() >> 12); }

}  // namespace AntiBurnIn
