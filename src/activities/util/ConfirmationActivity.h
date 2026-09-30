#pragma once
#include <functional>
#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ConfirmationActivity : public Activity {
 private:
  std::string popupTitle;
  OptionPopup confirmPopup;
  bool ignoreConfirmRelease = false;
  bool overrideDisabledReaderTouchscreen = false;
  const char* confirmLabel = nullptr;  // null: "Confirm"
  bool confirmFocused = false;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, bool ignoreInitialConfirmRelease = false,
                       bool overrideDisabledReaderTouchscreen = false);

  // Relabels the confirm option (e.g. "Retry") and optionally focuses it, so
  // the Confirm button picks it. Call before the activity starts.
  void setConfirmOption(const char* label, bool focused) {
    confirmLabel = label;
    confirmFocused = focused;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
