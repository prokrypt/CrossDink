#include "BugReportActivity.h"

#include <I18n.h>

#include <cstdio>

namespace {
constexpr int OPTION_SCREENSHOT = 0;
constexpr int OPTION_LOG = 1;
constexpr int OPTION_SAVE = 2;
}  // namespace

void BugReportActivity::showPopup(const int index) {
  char shot[64];
  char log[64];
  snprintf(shot, sizeof(shot), "[%c] %s", includeScreenshot ? 'x' : ' ', tr(STR_INCLUDE_SCREENSHOT));
  snprintf(log, sizeof(log), "[%c] %s", includeLog ? 'x' : ' ', tr(STR_INCLUDE_LOG));
  const char* options[] = {shot, log, tr(STR_SAVE_REPORT)};
  popup.setShowTappedRowFirst(false);
  popup.show(tr(STR_BUG_REPORT), options, 3, index, [this](const int idx) { picked = idx; });
  popup.setPrimaryOptionIndex(OPTION_SAVE);
  // No screenshot was saved: nothing to include.
  if (!ctx.hasScreenshot) popup.setDisabledOptions({true, false, false});
}

void BugReportActivity::onEnter() {
  Activity::onEnter();
  showPopup(OPTION_SAVE);
  requestUpdate(true);
}

void BugReportActivity::render(RenderLock&&) { popup.processRender(renderer, mappedInput); }

void BugReportActivity::loop() {
  if (popup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  const int idx = picked;
  picked = -1;
  if (idx == OPTION_SCREENSHOT || idx == OPTION_LOG) {
    if (idx == OPTION_SCREENSHOT) includeScreenshot = !includeScreenshot;
    if (idx == OPTION_LOG) includeLog = !includeLog;
    showPopup(idx);
    requestUpdate();
    return;
  }
  if (idx == OPTION_SAVE) {
    // Writes report.txt and hands the log and zip to a worker; BugReport::loop()
    // toasts the outcome over the screen below, once this popup is gone.
    const bool ok = BugReport::save(ctx, text, includeScreenshot, includeLog);
    setResult(OptionSelectionResult{static_cast<uint8_t>(ok ? 1 : 0)});
  } else {
    // Back or a tap outside the popup.
    BugReport::discard(ctx);
    ActivityResult cancelled;
    cancelled.isCancelled = true;
    setResult(std::move(cancelled));
  }
  finish();
}
