#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "components/OptionPopup.h"

class ConfirmationActivity : public Activity {
 private:
  std::string popupTitle;
  OptionPopup confirmPopup;
  bool overrideDisabledReaderTouchscreen = false;
  const char* confirmLabel = nullptr;  // null: "Confirm"
  const char* cancelLabel = nullptr;   // null: "Cancel"
  bool confirmFocused = false;
  const char* noteLabel = nullptr;
  char noteBody[24] = "";  // fixed: the popup holds a pointer to it
  bool (*notePoll)(void* ctx, std::string& body) = nullptr;
  void* notePollCtx = nullptr;

 public:
  ConfirmationActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& heading,
                       const std::string& body, bool overrideDisabledReaderTouchscreen = false);

  // Relabels the confirm option (e.g. "Retry") and optionally focuses it, so
  // the Confirm button picks it. Call before the activity starts.
  void setConfirmOption(const char* label, bool focused) {
    confirmLabel = label;
    confirmFocused = focused;
  }
  // Relabels the cancel option (e.g. "Stay here"). Call before the activity starts.
  void setCancelOption(const char* label) { cancelLabel = label; }

  // Bold-label note under the question ("Size: 1.6 MB"). poll, when set, runs
  // each loop and returns true after rewriting body; the note then redraws.
  // Call before the activity starts.
  void setNote(const char* label, const char* body, bool (*poll)(void* ctx, std::string& body) = nullptr,
               void* ctx = nullptr) {
    noteLabel = label;
    snprintf(noteBody, sizeof(noteBody), "%s", body);
    notePoll = poll;
    notePollCtx = ctx;
  }

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  bool drawsOverSourceFrame() const override { return true; }
};
