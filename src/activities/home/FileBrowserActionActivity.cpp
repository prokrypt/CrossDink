#include "FileBrowserActionActivity.h"

#include <I18n.h>

#include <algorithm>

FileBrowserActionActivity::FileBrowserActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string title, std::vector<MenuItem> items)
    : Activity("FileBrowserAction", renderer, mappedInput), title(std::move(title)), items(std::move(items)) {}

void FileBrowserActionActivity::onEnter() {
  Activity::onEnter();
  // Context menus are modal overlays: a tap on the dimmed image/list area
  // should close the menu instead of requiring the Back button.
  optionPopup.setDismissOnOutsideTouchDown(true);
  optionLabels.resize(items.size());
  std::transform(items.begin(), items.end(), optionLabels.begin(), [](const MenuItem& item) {
    std::string label = I18N.get(item.labelId);
    if (item.valueId) {
      label += ": ";
      label += I18N.get(*item.valueId);
    }
    return label;
  });
  optionPopup.setShowTappedRowFirst(false);  // ActivityManager drew the tapped row under us
  optionPopup.show(title.c_str(), optionLabels, 0, [this](const int index) {
    if (index < 0 || index >= static_cast<int>(items.size())) return;
    selectionMade = true;
    setResult(FileBrowserActionResult{static_cast<int>(items[index].action)});
    finish();
  });
  requestUpdate();
}

void FileBrowserActionActivity::finishCancelled() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void FileBrowserActionActivity::loop() {
  optionPopup.handleInput(mappedInput, [this] { requestUpdate(); });
  if (!optionPopup.isActive() && !selectionMade) finishCancelled();
}

void FileBrowserActionActivity::render(RenderLock&&) { optionPopup.processRender(renderer, mappedInput); }
