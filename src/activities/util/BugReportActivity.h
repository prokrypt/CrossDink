#pragma once

#include <string>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/BugReport.h"

// Last step of a bug report: a popup over the reported screen with
// "Include screenshot" / "Include log" toggles and Save. Back discards it.
class BugReportActivity final : public Activity {
  BugReport::Context ctx;
  std::string text;
  OptionPopup popup;
  bool includeScreenshot = true;
  bool includeLog = true;
  int picked = -1;  // the popup's last selection, handled in loop()

  void showPopup(int index);

 public:
  BugReportActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const BugReport::Context& ctx,
                    std::string text)
      : Activity("BugReport", renderer, mappedInput), ctx(ctx), text(std::move(text)) {
    includeScreenshot = ctx.hasScreenshot;
  }

  void onEnter() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool drawsOverSourceFrame() const override { return true; }
};
