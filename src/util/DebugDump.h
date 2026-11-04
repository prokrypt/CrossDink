#pragma once

#include <string>

// Settings and font list for log downloads and bug reports. Numeric settings
// only (by web API key) and font names: nothing in it can be a secret. Lists the
// SD font folders, so build it at download/report time, never on a hot path.
namespace DebugDump {
// "=== " lines, ending in '\n'.
std::string format();
}  // namespace DebugDump
