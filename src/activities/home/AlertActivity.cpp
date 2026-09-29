#include "AlertActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "CrossPointState.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void AlertActivity::onEnter() {
  Activity::onEnter();
  title = APP_STATE.pendingAlertTitle;
  body = APP_STATE.pendingAlertBody;
  goHomeOnBack = APP_STATE.pendingAlertGoHomeOnBack.exchange(false, std::memory_order_relaxed);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("ALERT", "Alert screen could not be rendered synchronously");
    requestUpdate();
  }
}

void AlertActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    if (goHomeOnBack) {
      onGoHome();
    } else {
      finish();
    }
  }
}

void AlertActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto contentWidth = pageWidth - 2 * metrics.contentSidePadding;
  const auto x = metrics.contentSidePadding;
  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title.c_str(), false);
  } else {
    GUI.drawHeader(renderer, header, title.c_str());
  }

  int y = header.y + header.height + metrics.verticalSpacing;

  auto bodyLines = renderer.wrappedText(UI_10_FONT_ID, body.c_str(), contentWidth, 10);
  for (const auto& line : bodyLines) {
    renderer.drawText(UI_10_FONT_ID, x, y, line.c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(
      goHomeOnBack ? mappedInput.withBackArrow(tr(STR_HOME)) : mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
