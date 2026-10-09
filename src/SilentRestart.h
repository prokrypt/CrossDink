#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// ESP.restart() with an RTC_NOINIT flag that survives the reboot, so setup()
// skips the boot splash and routes straight to a destination. Used to clear
// heap fragmentation accumulated during a wifi session.

enum class NetworkBootTarget : uint32_t {
  OTA = 2,
  OPDS = 3,
  KOREADER_SYNC = 4,
  KOREADER_AUTH = 5,
  FILE_TRANSFER = 6,
  MANAGE_FONTS = 7,
};

// NetworkBootTarget::OPDS payload for the server list instead of one server.
constexpr uint32_t OPDS_SERVER_LIST_PAYLOAD = UINT32_MAX;

constexpr bool isNetworkBootTargetValue(const uint32_t value) {
  switch (static_cast<NetworkBootTarget>(value)) {
    case NetworkBootTarget::OTA:
    case NetworkBootTarget::OPDS:
    case NetworkBootTarget::KOREADER_SYNC:
    case NetworkBootTarget::KOREADER_AUTH:
    case NetworkBootTarget::FILE_TRANSFER:
    case NetworkBootTarget::MANAGE_FONTS:
      return true;
  }
  return false;
}

static_assert(isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::OTA)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::OPDS)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::KOREADER_SYNC)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::KOREADER_AUTH)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::FILE_TRANSFER)) &&
                  isNetworkBootTargetValue(static_cast<uint32_t>(NetworkBootTarget::MANAGE_FONTS)),
              "Every network boot target must pass RTC target validation");

void silentRestart();  // home screen
// Plain ESP.restart() (no silent token) that keeps the current frame in PSRAM,
// so the next boot's first paint is a Fast refresh. Used after a firmware flash.
void restartKeepingPanelFrame();
void silentRestartToReader(bool cleanImageBaseOnEntry = false);  // currently-open EPUB (APP_STATE.openEpubPath)
// Reboots immediately after an activity releases exclusive raw storage.
void restartToHomeAfterStorageHandoff();
// Opens a Wi-Fi screen. Replaces the current screen with a handoff that, once
// the old screen has freed its state, opens the target in place when the
// internal heap allows and otherwise reboots into a minimal network boot.
void silentRestartToNetwork(NetworkBootTarget target, uint32_t payload = 0);
void silentRestartToManageFonts();
// Ends a Wi-Fi session without rebooting: Wi-Fi is stopped and deinitialized,
// and after a minimal network boot the reader resources it skipped are set up.
// False when the largest internal heap block left is too small for reader
// work (a lower bar when goingHome); the caller then falls back to its silent
// restart. True during deep sleep, so callers go on with their normal cleanup.
bool leaveNetworkInPlace(bool goingHome = false);
// Debug builds (CROSSDINK_PERF_LOG): logs the small used blocks that split
// internal RAM's free runs, with the owning task when one holds a TCB there.
void logInternalHeapPins(const char* why);
// True when a leaving Wi-Fi screen should keep the station link for the Goodies
// Wi-Fi remote (same network, remote on). Always false without Goodies.
bool keepWifiForRemote();
// For a Wi-Fi screen's onExit(): runs leaveNetworkInPlace() once the screen is
// destroyed (finishNetworkExit()), so its leftover allocations don't split the
// block the check needs. If that fails it restarts into `bookPath`, or Home
// when empty.
void leaveNetworkAfterExit(std::string bookPath);
// ActivityManager, after destroying a screen: runs a pending leaveNetworkAfterExit().
void finishNetworkExit();
// Reboots to home, then opens SD Card Firmware Update for `firmwarePath` (which
// still asks for confirmation). Paths of MAX_SILENT_FIRMWARE_PATH bytes or more
// fall back to a plain silentRestart().
constexpr size_t MAX_SILENT_FIRMWARE_PATH = 128;
void silentRestartToFirmwareUpdate(const std::string& firmwarePath);
// Returns the path armed by silentRestartToFirmwareUpdate() once, then clears it.
std::string consumeSilentRestartFirmwareUpdate();

void armSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t spineIndex, uint16_t targetPage,
                                     bool autoPageTurnActive);
bool consumeSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t& spineIndex, uint16_t& targetPage,
                                         bool& autoPageTurnActive);
