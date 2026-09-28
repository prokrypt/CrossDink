#pragma once

// PlatformIO normally supplies these through build_flags/extra_scripts. Keep
// fallbacks here so editor indexers and simulator-like tools still parse files.
#ifndef CROSSDINK_VERSION
#define CROSSDINK_VERSION "dev"
#endif

#ifndef CROSSDINK_PIOENV
#define CROSSDINK_PIOENV "unknown"
#endif

#ifndef CROSSDINK_BUILD_ENV
#define CROSSDINK_BUILD_ENV "unknown"
#endif

#ifndef CROSSDINK_FIRMWARE_DEVICE_TYPE
#define CROSSDINK_FIRMWARE_DEVICE_TYPE "unknown"
#endif
