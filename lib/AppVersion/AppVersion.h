#pragma once

// The version carries the branch and commit in test builds, so
// scripts/git_branch.py defines it only for src/util/BuildInfo.cpp, which
// implements these. A global define would force a full rebuild per commit.
namespace AppVersion {
const char* version();       // e.g. "1.6.0-x4-pro"
const char* versionLabel();  // "CrossDink <version>"
const char* userAgent();     // "CrossDink-ESP32-<version>"
}  // namespace AppVersion

// PlatformIO normally supplies these through build_flags/extra_scripts. Keep
// fallbacks here so editor indexers and simulator-like tools still parse files.

#ifndef CROSSDINK_PIOENV
#define CROSSDINK_PIOENV "unknown"
#endif

#ifndef CROSSDINK_BUILD_ENV
#define CROSSDINK_BUILD_ENV "unknown"
#endif

#ifndef CROSSDINK_FIRMWARE_DEVICE_TYPE
#define CROSSDINK_FIRMWARE_DEVICE_TYPE "unknown"
#endif
