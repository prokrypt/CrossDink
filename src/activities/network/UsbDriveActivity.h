#pragma once

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "util/TransferLightPulse.h"

class UsbDriveActivity final : public Activity {
 public:
  UsbDriveActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("UsbDrive", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool powerOffPanelWhenIdle() const override { return true; }
  bool preventAutoSleep() override {
    return state == State::Connected || state == State::Accessed || forcedDisconnectRequested ||
           (!startFailed && state == State::IoError);
  }
  bool requiresExclusiveStorageLoop() const override { return true; }

 private:
  enum class State { Unsupported, WaitingForHost, Connected, Accessed, Ejected, Disconnected, IoError };

  static constexpr unsigned long HOST_WAIT_TIMEOUT_MS = 5UL * 60UL * 1000UL;
  static constexpr unsigned long START_FAILURE_TIMEOUT_MS = 30UL * 1000UL;
  static constexpr unsigned long FORCED_DISCONNECT_TIMEOUT_MS = 1000UL;
  static constexpr unsigned long HOST_SUSPEND_TIMEOUT_MS = 2000UL;

  // Host I/O newer than this counts as a transfer for the light.
  static constexpr uint32_t IO_ACTIVE_MS = 500;
  // Bursts under this are host polling; they are summarized, not logged each.
  static constexpr uint32_t BURST_LOG_MIN_BYTES = 64 * 1024;
  static constexpr uint32_t POLL_SUMMARY_MS = 30UL * 1000UL;
  // The host's mount scan counts as done after this long with no I/O.
  static constexpr uint32_t MOUNT_SETTLE_MS = 1000;

  void restartToHome();
  void updateTransferLight();
  void renderMessage(const char* message, const char* detail = nullptr) const;

  State state = State::Unsupported;
  bool preparing = true;
  bool startFailed = false;
  bool restartRequested = false;
  bool forcedDisconnectRequested = false;
  bool hostSuspendPending = false;
  unsigned long hostWaitStartedAt = 0;
  unsigned long startFailureStartedAt = 0;
  unsigned long forcedDisconnectRequestedAt = 0;
  unsigned long hostSuspendStartedAt = 0;
  ScreenTransitionRefresh screenTransitionRefresh;
  TransferLightPulse transferLight;
  // Current burst of host I/O, for the [FL] log.
  bool ioBurst = false;
  uint32_t burstStartMs = 0;
  uint32_t burstReadStart = 0;
  uint32_t burstWriteStart = 0;
  uint16_t pollBursts = 0;
  uint32_t pollSummaryAt = 0;
  // Mount timing for the [USB] mount line.
  uint32_t enterMs = 0;
  uint32_t hostMs = 0;
  bool mountLogged = false;
};
