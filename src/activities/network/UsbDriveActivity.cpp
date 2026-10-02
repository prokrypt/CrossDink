#include "UsbDriveActivity.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <SdCardFontSystem.h>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "platform/UsbSerialJtagHandoff.h"
#include "util/BatteryLog.h"

void UsbDriveActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::event("xfer_start", "usb-drive");
  enterMs = millis();
  hostMs = 0;
  mountLogged = false;
  state = State::Unsupported;
  preparing = true;
  startFailed = false;
  restartRequested = false;
  forcedDisconnectRequested = false;
  hostSuspendPending = false;
  hostWaitStartedAt = 0;
  startFailureStartedAt = 0;
  forcedDisconnectRequestedAt = 0;
  hostSuspendStartedAt = 0;
  ioBurst = false;
  gateOpen = false;
  gateWindowMs = millis();
  gateBytes = 0;
  pollBursts = 0;
  pollSummaryAt = millis();

  // Paint the instruction screen before detaching the filesystem and exposing
  // its block device to the host. The two operations must never overlap.
  requestUpdateAndWait();
#ifndef SIMULATOR
  // The host can replace fonts without going through firmware file APIs.
  sdFontSystem.markRegistryDirty();
  if (!Storage.beginUsbDrive()) {
    LOG_ERR("USB", "Unable to start USB Drive");
    preparing = false;
    startFailed = true;
    state = State::IoError;
    startFailureStartedAt = millis();
    requestUpdate();
    return;
  }

  transferLight.begin();
#endif
  preparing = false;
  state = State::WaitingForHost;
  hostWaitStartedAt = millis();
  requestUpdate();
}

void UsbDriveActivity::onExit() {
  BatteryLog::event("xfer_end", "usb-drive");
  transferLight.end();  // restores the user's brightness
  library::invalidateLibraryIndex();
#ifndef SIMULATOR
  if (!restartRequested) Storage.endUsbDrive();
#endif
  Activity::onExit();
}

void UsbDriveActivity::updateTransferLight() {
#ifndef SIMULATOR
  UsbDriveIo io;
  if (restartRequested || !Storage.usbDriveIo(io)) return;
  const uint32_t now = millis();
  const bool active = io.lastIoMs != 0 && now - io.lastIoMs < IO_ACTIVE_MS;
  const uint32_t bytes = io.readBytes + io.writeBytes;
  if (now - gateWindowMs >= GATE_WINDOW_MS) {
    gateWindowMs = now;
    gateBytes = bytes;
  }
  if (!active) {
    gateOpen = false;
  } else if (!gateOpen && bytes - gateBytes >= GATE_MIN_BYTES) {
    gateOpen = true;
    LOG_DBG("FL", "usb gate open: %lu KB in %lu ms", static_cast<unsigned long>((bytes - gateBytes) / 1024),
            static_cast<unsigned long>(now - gateWindowMs));
  }
  transferLight.update(gateOpen);

  if (!mountLogged && io.firstIoMs != 0 && !active && now - io.lastIoMs >= MOUNT_SETTLE_MS) {
    mountLogged = true;
    const uint32_t hostAt = hostMs != 0 ? hostMs : io.firstIoMs;
    const uint32_t hostToIo = io.firstIoMs > hostAt ? io.firstIoMs - hostAt : 0;
    LOG_INF("USB", "mount enter->host=%lu ms host->first_io=%lu ms first_io->settled=%lu ms (%lu KB, %lu ops)",
            static_cast<unsigned long>(hostAt - enterMs), static_cast<unsigned long>(hostToIo),
            static_cast<unsigned long>(io.lastIoMs - io.firstIoMs),
            static_cast<unsigned long>((io.readBytes + io.writeBytes) / 1024), static_cast<unsigned long>(io.ops));
  }

  if (active && !ioBurst) {
    ioBurst = true;
    burstStartMs = io.lastIoMs;
    burstReadStart = io.readBytes;
    burstWriteStart = io.writeBytes;
  } else if (!active && ioBurst) {
    ioBurst = false;
    const uint32_t readKb = (io.readBytes - burstReadStart) / 1024;
    const uint32_t writeKb = (io.writeBytes - burstWriteStart) / 1024;
    if ((readKb + writeKb) * 1024 >= BURST_LOG_MIN_BYTES) {
      LOG_DBG("FL", "usb burst %lu ms r=%luKB w=%luKB", static_cast<unsigned long>(io.lastIoMs - burstStartMs),
              static_cast<unsigned long>(readKb), static_cast<unsigned long>(writeKb));
    } else {
      pollBursts++;
    }
  }
  // Host idle polling pulses the light too; one summary line per window.
  if (pollBursts > 0 && now - pollSummaryAt >= POLL_SUMMARY_MS) {
    LOG_DBG("FL", "usb idle polls: %u in last %lu s", pollBursts,
            static_cast<unsigned long>((now - pollSummaryAt) / 1000));
    pollBursts = 0;
    pollSummaryAt = now;
  }
#endif
}

void UsbDriveActivity::loop() {
  updateTransferLight();
#ifndef SIMULATOR
  if (!startFailed) {
    const auto storageState = Storage.usbDriveState();
    const State nextState = static_cast<State>(storageState);
    if (nextState != state) {
      if (hostMs == 0 && (nextState == State::Connected || nextState == State::Accessed)) hostMs = millis();
      const bool messageChanged = state != State::Connected || nextState != State::Accessed;
      state = nextState;
      if (messageChanged) requestUpdate();
    }
  }
#endif

// The simulator HAL does not expose TinyUSB's host-suspend signal.
#if !defined(SIMULATOR)
  // Cable removal can leave a battery-powered USB device mounted but
  // suspended. A sleeping host intentionally follows the same timeout policy.
  if ((state == State::Connected || state == State::Accessed) && Storage.usbDriveHostSuspended()) {
    if (!hostSuspendPending) {
      hostSuspendPending = true;
      hostSuspendStartedAt = millis();
    } else if (millis() - hostSuspendStartedAt >= HOST_SUSPEND_TIMEOUT_MS) {
      LOG_INF("USB", "USB Drive host suspend timed out; ending session");
      restartToHome();
      return;
    }
  } else {
    hostSuspendPending = false;
  }
#endif

  if (state == State::WaitingForHost && millis() - hostWaitStartedAt >= HOST_WAIT_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive host wait timed out");
    restartToHome();
    return;
  }

  if (startFailed && millis() - startFailureStartedAt >= START_FAILURE_TIMEOUT_MS) {
    LOG_INF("USB", "USB Drive startup failure timed out");
    restartToHome();
    return;
  }

  if (forcedDisconnectRequested) {
    if (millis() - forcedDisconnectRequestedAt >= FORCED_DISCONNECT_TIMEOUT_MS) {
      LOG_ERR("USB", "USB Drive host disconnect grace period ended; forcing restart");
      restartToHome();
    }
    return;
  }

  if (!startFailed && state == State::IoError) {
    forcedDisconnectRequested = true;
    forcedDisconnectRequestedAt = millis();
    LOG_ERR("USB", "USB Drive I/O error; disconnecting host");
#ifndef SIMULATOR
    if (!Storage.disconnectUsbDriveHost()) {
      LOG_ERR("USB", "Unable to request USB Drive host disconnect");
    }
#endif
    return;
  }

  const bool canExitWithInput = state == State::WaitingForHost || state == State::IoError;
  if (canExitWithInput) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Power) || mappedInput.wasHomeGesture()) {
      restartToHome();
      return;
    }
    if (state == State::WaitingForHost) return;
  }

  if (state == State::Ejected || state == State::Disconnected || state == State::Unsupported) {
    restartToHome();
  }
}

void UsbDriveActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const char* title = tr(STR_USB_DRIVE);
  const bool canExitWithInput = state == State::WaitingForHost || state == State::IoError;
  if (mappedInput.hasTouchHardware() && canExitWithInput) {
    TouchHeaderBackButton::drawCompact(renderer, title);
  } else {
    CompactHeader::drawTitle(renderer, title);
  }

  if (preparing) {
    renderMessage(tr(STR_USB_DRIVE_PREPARING), tr(STR_USB_DRIVE_EJECT_HINT));
  } else if (forcedDisconnectRequested) {
    renderMessage(tr(STR_USB_DRIVE_ERROR));
  } else
    switch (state) {
      case State::WaitingForHost:
        renderMessage(tr(STR_USB_DRIVE_WAITING));
        break;
      case State::Connected:
      case State::Accessed:
        renderMessage(tr(STR_USB_DRIVE_CONNECTED), tr(STR_USB_DRIVE_EJECT_HINT));
        break;
      case State::IoError:
        renderMessage(startFailed ? tr(STR_USB_DRIVE_START_ERROR) : tr(STR_USB_DRIVE_ERROR));
        break;
      case State::Ejected:
      case State::Disconnected:
      case State::Unsupported:
        break;
    }

  if (state == State::WaitingForHost || state == State::IoError) {
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(0));
}

void UsbDriveActivity::renderMessage(const char* message, const char* detail) const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect textArea{metrics.contentSidePadding, 0, renderer.getScreenWidth() - metrics.contentSidePadding * 2,
                      renderer.getScreenHeight()};
  int y = renderer.getScreenHeight() / 2 - renderer.getLineHeight(UI_10_FONT_ID);
  y += UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, y, message, 2, true, EpdFontFamily::BOLD);
  if (detail) {
    y += metrics.verticalSpacing;
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, y, detail, 3);
  }
}

void UsbDriveActivity::restartToHome() {
  if (restartRequested) return;
  restartRequested = true;
  transferLight.end();
#ifndef SIMULATOR
  Storage.endUsbDrive();
#endif
#ifdef SIMULATOR
  activityManager.goHome();
#else
  delay(20);
  handoffUsbOtgToSerialJtag();
  restartToHomeAfterStorageHandoff();
#endif
}
