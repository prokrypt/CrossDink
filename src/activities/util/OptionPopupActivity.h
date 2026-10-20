#pragma once
#include <I18n.h>

#include <string>
#include <utility>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

// Option list drawn as a popup over the caller's frame. Result: OptionSelectionResult.
class OptionPopupActivity : public Activity {
 private:
  OptionPopup popup;
  StrId titleId;
  std::vector<std::string> options;
  int currentIndex;

 public:
  OptionPopupActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, StrId titleId,
                      std::vector<std::string> options, int currentIndex)
      : Activity("OptionPopup", renderer, mappedInput),
        titleId(titleId),
        options(std::move(options)),
        currentIndex(currentIndex) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool drawsOverSourceFrame() const override { return true; }
};
