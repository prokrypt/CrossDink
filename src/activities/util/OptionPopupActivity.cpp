#include "OptionPopupActivity.h"

#include "activities/ActivityResult.h"

void OptionPopupActivity::onEnter() {
  Activity::onEnter();
  popup.setShowTappedRowFirst(false);  // ActivityManager drew the tapped row under us
  popup.show(titleId, options, currentIndex, [this](int idx) {
    ActivityResult res;
    res.data = OptionSelectionResult{static_cast<uint8_t>(idx)};
    setResult(std::move(res));
    finish();
  });
  requestUpdate(true);
}

void OptionPopupActivity::render(RenderLock&&) { popup.processRender(renderer, mappedInput); }

void OptionPopupActivity::loop() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  // Dismissed without a selection (Back button or tap outside).
  ActivityResult res;
  res.isCancelled = true;
  setResult(std::move(res));
  finish();
}
