#include "ConfirmationActivity.h"

#include <I18n.h>

#include "components/UITheme.h"
#include "fontIds.h"

ConfirmationActivity::ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::string& heading, const std::string& body,
                                           bool ignoreInitialConfirmRelease, bool overrideDisabledReaderTouchscreen)
    : Activity("Confirmation", renderer, mappedInput),
      ignoreConfirmRelease(ignoreInitialConfirmRelease),
      overrideDisabledReaderTouchscreen(overrideDisabledReaderTouchscreen) {
  popupTitle.reserve(heading.size() + body.size() + 1);
  popupTitle = heading;
  if (!heading.empty() && !body.empty()) {
    popupTitle += ' ';
  }
  popupTitle += body;
}

void ConfirmationActivity::onEnter() {
  Activity::onEnter();
  if (overrideDisabledReaderTouchscreen) {
    mappedInput.setReaderTouchscreenOverride(true);
  }

  const char* options[] = {I18N.get(StrId::STR_CANCEL), confirmLabel ? confirmLabel : I18N.get(StrId::STR_CONFIRM)};
  confirmPopup.setShowTappedRowFirst(false);  // ActivityManager drew the tapped row under us
  confirmPopup.show(
      popupTitle.c_str(), options, 2, confirmFocused ? 1 : 0,
      [this](int idx) {
        ActivityResult res;
        res.isCancelled = (idx != 1);
        setResult(std::move(res));
        finish();
      },
      OptionPopup::Note(noteLabel, noteLabel ? noteBody : nullptr));
  confirmPopup.setPrimaryOptionIndex(1);

  requestUpdate(true);
}

void ConfirmationActivity::onExit() {
  if (overrideDisabledReaderTouchscreen) {
    mappedInput.setReaderTouchscreenOverride(false);
  }
  Activity::onExit();
}

void ConfirmationActivity::render(RenderLock&&) {
  if (confirmPopup.processRender(renderer, mappedInput)) return;
}

void ConfirmationActivity::loop() {
  if (ignoreConfirmRelease) {
    const bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    if (confirmReleased || !mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      ignoreConfirmRelease = false;
      return;
    }
  }

  if (notePoll) {
    std::string body = noteBody;
    if (notePoll(notePollCtx, body)) {
      // No RenderLock from the loop (a blocking take here froze the device
      // when the size landed): the popup keeps pointing at noteBody, so a
      // render racing this copy draws at worst one mixed frame before the
      // redraw requested below.
      snprintf(noteBody, sizeof(noteBody), "%s", body.c_str());
      confirmPopup.setNote(OptionPopup::Note(noteLabel, noteBody));
      requestUpdate();
    }
  }

  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Popup dismissed without a selection (Back button or tap outside): cancel.
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
