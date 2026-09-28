#include "SdFirmwareUpdateActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_ota_ops.h>
#ifndef SIMULATOR
#include <FreeInkDisplay.h>
#endif

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/FirmwareFlasher.h"

namespace {
// Each progress repaint competes with the flash loop for the CPU and, on
// shared-bus boards, the SPI bus. With the keyboard DU LUT a repaint is short
// enough for 5% steps.
constexpr unsigned int PROGRESS_STEP_PERCENT = 5;
// DU frames for progress repaints (the keyboard's tested "63 6" setting).
constexpr uint8_t PROGRESS_LUT_FRAMES = 6;

unsigned int progressStep(size_t written, size_t total) {
  if (total == 0) return 0;
  const auto pct = static_cast<unsigned int>((static_cast<uint64_t>(written) * 100) / total);
  return pct - pct % PROGRESS_STEP_PERCENT;
}
}  // namespace

void SdFirmwareUpdateActivity::onEnter() {
  Activity::onEnter();
  // Build-identity marker — confirms which firmware build owns the SD update flow.
  LOG_INF("FW", "SdFirmwareUpdateActivity build=%s %s recovery=%d", __DATE__, __TIME__, recoveryMode ? 1 : 0);
  state = State::PICKING;
  // A preselected file is validated from loop(): selectFirmware() waits on the
  // render task, which must not happen inside onEnter().
  if (preselectedPath.empty()) launchPicker();
}

void SdFirmwareUpdateActivity::onExit() {
  Activity::onExit();
  flashLight.end();
#ifndef SIMULATOR
  freeink::setUc8179KbdExperiment(nullptr);  // render() may have left the progress LUT on
#endif
}

void SdFirmwareUpdateActivity::launchPicker() {
  // Reuse the standard file browser, restricted to .bin files only. Start in
  // /firmware (where the upload script puts builds) when it exists.
  const char* startDir = Storage.exists("/firmware") ? "/firmware" : "/";
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, startDir, FileBrowserActivity::Mode::PickFirmware),
      [this](const ActivityResult& result) { onPickerResult(result); });
}

void SdFirmwareUpdateActivity::onPickerResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Recovery mode: re-launch the picker so the user cannot escape into a half-initialised UI.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  const auto* path = std::get_if<FilePathResult>(&result.data);
  if (!path) {
    LOG_ERR("FW", "Picker returned no path");
    finish();
    return;
  }
  selectFirmware(path->path);
}

void SdFirmwareUpdateActivity::selectFirmware(std::string path) {
  firmwarePath = std::move(path);
  LOG_DBG("FW", "Selected: %s", firmwarePath.c_str());

  {
    RenderLock lock(*this);
    state = State::VALIDATING;
  }
  requestUpdateAndWait();

  if (!validateFirmware()) {
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  promptConfirmation();
}

bool SdFirmwareUpdateActivity::validateFirmware() {
  HalFile file;
  if (!Storage.openFileForRead("FW", firmwarePath.c_str(), file) || !file) {
    errorMessage = tr(STR_FIRMWARE_FILE_OPEN_FAILED);
    return false;
  }
  firmwareSize = file.fileSize();
  file.close();

  // Resolve the next-update partition directly via the OTA API. Previously this
  // probed via Update.begin(firmwareSize)/Update.abort() to learn the partition
  // size, which had the side effect of erasing partition state and was wasted
  // work since we only need the size bound for validation here.
  const esp_partition_t* dest = esp_ota_get_next_update_partition(nullptr);
  if (!dest) {
    LOG_ERR("FW", "no next-update partition available");
    errorMessage = tr(STR_INVALID_FIRMWARE);
    return false;
  }
  const size_t partitionLimit = dest->size;
  if (firmwareSize > partitionLimit) {
    LOG_ERR("FW", "firmware (%u bytes) exceeds partition (%u bytes)", static_cast<unsigned>(firmwareSize),
            static_cast<unsigned>(partitionLimit));
    errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    return false;
  }

  // Header only (magic, chip, segment table and sizes): a truncated or foreign
  // .bin fails here without reading the whole card file. The checksum, SHA-256
  // and board tag are verified during the flash, which only activates the new
  // slot when they pass.
  const auto vr = firmware_flash::checkImageHeaderFile(firmwarePath.c_str(), partitionLimit);
  if (vr != firmware_flash::Result::OK) {
    LOG_ERR("FW", "image validation failed: %s", firmware_flash::resultName(vr));
    if (vr == firmware_flash::Result::TOO_LARGE) {
      errorMessage = tr(STR_FIRMWARE_TOO_LARGE);
    } else if (vr == firmware_flash::Result::TOO_SMALL) {
      errorMessage = tr(STR_FIRMWARE_TOO_SMALL);
    } else if (vr == firmware_flash::Result::BAD_CHIP || vr == firmware_flash::Result::WRONG_BOARD) {
      errorMessage = tr(STR_FIRMWARE_WRONG_DEVICE);
    } else {
      errorMessage = tr(STR_INVALID_FIRMWARE);
    }
    return false;
  }
  return true;
}

void SdFirmwareUpdateActivity::promptConfirmation() {
  {
    RenderLock lock(*this);
    state = State::CONFIRMING;
  }
  // Show "Update firmware?" with the file path as the body line.
  std::string heading = tr(STR_FIRMWARE_UPDATE_PROMPT);
  // Use the basename only to keep the body short.
  std::string body = firmwarePath;
  const auto pos = body.find_last_of('/');
  if (pos != std::string::npos) body = body.substr(pos + 1);

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, heading, body),
                         [this](const ActivityResult& result) { onConfirmationResult(result); });
}

void SdFirmwareUpdateActivity::onConfirmationResult(const ActivityResult& result) {
  if (result.isCancelled) {
    if (recoveryMode) {
      // Go back to the picker rather than exiting recovery.
      launchPicker();
      return;
    }
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state = State::UPDATING;
    writtenBytes = 0;
    lastRenderedPercent = 101;
  }
  requestUpdateAndWait();
  performUpdate();
}

void SdFirmwareUpdateActivity::performUpdate() {
  LOG_INF("FW", "SD update: %s (%u bytes)", firmwarePath.c_str(), static_cast<unsigned>(firmwareSize));

  auto progressCb = +[](size_t written, size_t total, void* ctx) {
    auto* self = static_cast<SdFirmwareUpdateActivity*>(ctx);
    const bool stepChanged = progressStep(written, total) != progressStep(self->writtenBytes, total);
    self->writtenBytes = written;
    self->firmwareSize = total;
    // The loop is blocked for the whole flash, so the warble runs from here.
    self->flashLight.update(true);
    // immediate=true: wake the render task directly. We're in a tight sync
    // loop so the main loop won't drain the requestedUpdate flag for us.
    if (stepChanged) self->requestUpdate(true);
  };

  // SD is removable: the flash pass verifies the bytes it writes (checksum,
  // SHA-256, board tag) and activates the slot only when they pass.
  // Nothing reads touch while flashing; sleep the GT911 to save power. After a
  // failure the main loop's touch sleep policy wakes it again.
  if (gpio.hasTouch() && !gpio.setTouchSleep(true)) LOG_ERR("FW", "Touch controller did not sleep");
  flashLight.begin();
  const auto result = firmware_flash::flashFromSdPath(firmwarePath.c_str(), progressCb, this);
  if (result != firmware_flash::Result::OK) {
    LOG_ERR("FW", "flash failed: %s", firmware_flash::resultName(result));
    flashLight.end();
    errorMessage = result == firmware_flash::Result::BAD_CHIP || result == firmware_flash::Result::WRONG_BOARD
                       ? tr(STR_FIRMWARE_WRONG_DEVICE)
                       : tr(STR_FIRMWARE_WRITE_FAILED);
    RenderLock lock(*this);
    state = State::FAILED;
    requestUpdate();
    return;
  }

  LOG_INF("FW", "SD firmware update complete, restarting");
  {
    RenderLock lock(*this);
    state = State::SUCCESS;
  }
  // Held steady until the restart, so the light changing marks the reboot.
  flashLight.holdOn();
  requestUpdateAndWait();
  delay(1500);
  // Back to the user's light state so the silent restart restores that one.
  flashLight.end();
  restartKeepingPanelFrame();
}

void SdFirmwareUpdateActivity::loop() {
  if (!preselectedPath.empty()) {
    std::string path = std::move(preselectedPath);
    preselectedPath.clear();
    selectFirmware(std::move(path));
    return;
  }
  if (state == State::FAILED) {
    int x = 0;
    int y = 0;
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
        mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      if (recoveryMode) {
        // Go back to picker so user can try a different .bin
        state = State::PICKING;
        launchPicker();
        return;
      }
      finish();
    }
  }
}

void SdFirmwareUpdateActivity::render(RenderLock&&) {
#ifndef SIMULATOR
  // Trial (log item 9): progress repaints use the keyboard's windowed DU LUT
  // (~230 ms instead of the ~560 ms OTP fast refresh) while flashing, so each
  // one holds the flash loop's SPI/cache for less time. Off in every other state.
  if (state == State::UPDATING) {
    freeink::Uc8179KbdExperiment exp;
    exp.flags = freeink::Uc8179KbdExperiment::KbdLut | freeink::Uc8179KbdExperiment::WindowDrf;
    exp.lutFrames = PROGRESS_LUT_FRAMES;
    freeink::setUc8179KbdExperiment(&exp);
  } else {
    freeink::setUc8179KbdExperiment(nullptr);
  }
#endif
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  const char* headerText = recoveryMode ? tr(STR_RECOVERY_MODE) : tr(STR_SD_FIRMWARE_UPDATE);
  const Rect header{0, metrics.topPadding, pageWidth, TouchHeaderBackButton::height(metrics, mappedInput)};
  if (state == State::FAILED && mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, headerText, false);
  } else {
    GUI.drawHeader(renderer, header, headerText);
  }

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - lineHeight) / 2;
  const Rect textArea{metrics.contentSidePadding, 0, pageWidth - metrics.contentSidePadding * 2, pageHeight};

  if (state == State::VALIDATING) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_VALIDATING_FIRMWARE));
  } else if (state == State::UPDATING) {
    // Throttle redraws to once per progress step.
    const unsigned int pct = progressStep(writtenBytes, firmwareSize);
    if (pct == lastRenderedPercent) {
      return;
    }
    lastRenderedPercent = pct;

    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATING), true, EpdFontFamily::BOLD);

    int y = top + lineHeight + metrics.verticalSpacing;
    GUI.drawProgressBar(
        renderer,
        Rect{metrics.contentSidePadding, y, pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
        static_cast<int>(pct), 100);
    y += metrics.progressBarHeight + metrics.verticalSpacing;
    // Percent label is drawn by BaseTheme::drawProgressBar; this slot is left intentionally empty
    // so the do-not-power-off line below stays at the same Y as before.
    y += lineHeight + metrics.verticalSpacing;
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, y, tr(STR_FIRMWARE_UPDATE_DO_NOT_POWER_OFF), 2);
  } else if (state == State::SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_COMPLETE), true, EpdFontFamily::BOLD);
    UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
                                     tr(STR_RESTARTING_HINT), 3);
  } else if (state == State::FAILED) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_UPDATE_FAILED), true, EpdFontFamily::BOLD);
    if (!errorMessage.empty()) {
      UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top + lineHeight + metrics.verticalSpacing,
                                       errorMessage.c_str(), 3);
    }
    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    // PICKING / CONFIRMING: a sub-activity is on top, nothing to draw.
    if (recoveryMode) {
      UITheme::drawCenteredWrappedText(renderer, textArea, UI_10_FONT_ID, top, tr(STR_RECOVERY_MODE_HINT), 3);
    }
  }

  renderer.displayBuffer();
}
