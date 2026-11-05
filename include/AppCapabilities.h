#pragma once

// The app deliberately has a build-time touch capability, separate from a
// runtime probe. Button-only images must not retain the touch UI merely because
// the shared source tree also builds for Sticky.
#ifndef CROSSDINK_APP_CAP_TOUCH
#error "Define CROSSDINK_APP_CAP_TOUCH as 0 or 1 in the PlatformIO environment"
#endif

#if CROSSDINK_APP_CAP_TOUCH != 0 && CROSSDINK_APP_CAP_TOUCH != 1
#error "CROSSDINK_APP_CAP_TOUCH must be 0 or 1"
#endif

#ifndef CROSSDINK_APP_CAP_USB_DRIVE
#error "Define CROSSDINK_APP_CAP_USB_DRIVE as 0 or 1 in the PlatformIO environment"
#endif

#if CROSSDINK_APP_CAP_USB_DRIVE != 0 && CROSSDINK_APP_CAP_USB_DRIVE != 1
#error "CROSSDINK_APP_CAP_USB_DRIVE must be 0 or 1"
#endif

// Native simulator BoardConfig intentionally exposes only simulated runtime
// profiles, so keep this firmware-image identity available at the app layer.
#if defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC
#define CROSSDINK_APP_DEVICE_X4CLASSIC 1
#else
#define CROSSDINK_APP_DEVICE_X4CLASSIC 0
#endif

// Native simulator BoardConfig deliberately has no FREEINK_CAP_TOUCH macro.
// Firmware builds must keep the app and SDK capability selections in lockstep.
#if !defined(SIMULATOR)
#include <BoardConfig.h>
#if CROSSDINK_APP_CAP_TOUCH != FREEINK_CAP_TOUCH
#error "CROSSDINK_APP_CAP_TOUCH must match FREEINK_CAP_TOUCH"
#endif
#if CROSSDINK_APP_CAP_USB_DRIVE != FREEINK_CAP_USB_MSC
#error "CROSSDINK_APP_CAP_USB_DRIVE must match FREEINK_CAP_USB_MSC"
#endif
#endif

// Every button-only reader uses the full-screen menu and its bounded sample preview.
#define CROSSDINK_APP_READER_SAMPLE_PREVIEW (!CROSSDINK_APP_CAP_TOUCH)
