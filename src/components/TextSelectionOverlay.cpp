#include "TextSelectionOverlay.h"
#if CROSSDINK_APP_CAP_TOUCH
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "UIScale.h"
#include "UITheme.h"

namespace TextSelectionOverlay {
namespace fui = freeink::ui;
namespace {
const char* label(int index) {
  static constexpr StrId labels[] = {StrId::STR_LOOKUP_SHORT, StrId::STR_CLIP};
  return I18n::getInstance().get(labels[index]);
}
}  // namespace

Rect safeArea(const GfxRenderer& renderer) {
  const auto area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int x = std::max(area.x, left), y = std::max(area.y, top);
  return {static_cast<int16_t>(x), static_cast<int16_t>(y),
          static_cast<int16_t>(std::max(0, std::min(area.x + area.width, renderer.getScreenWidth() - right) - x)),
          static_cast<int16_t>(std::max(0, std::min(area.y + area.height, renderer.getScreenHeight() - bottom) - y))};
}

void placeActions(const GfxRenderer& renderer, Layout& layout, const int anchorX, const int textTop) {
  const auto safe = safeArea(renderer);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int font = uiScaleSpec().smallFontId;
  const int padding = metrics.menuSpacing;
  int labelWidth = 0;
  for (int i = 0; i < actionCount; ++i) labelWidth = std::max(labelWidth, renderer.getTextWidth(font, label(i)));
  const int width = std::min<int>(safe.width, actionCount * (labelWidth + padding * 2) + padding * (actionCount + 1));
  const int buttonWidth = std::max(1, (width - padding * (actionCount + 1)) / actionCount);
  const int lines = labelWidth > buttonWidth - padding * 2 ? 2 : 1;
  const int height = std::max(36, renderer.getLineHeight(font) * lines + padding * 2);
  const int x = std::clamp(anchorX - padding, static_cast<int>(safe.x), safe.right() - width);
  const int top = safe.y + std::max(metrics.topPadding, metrics.verticalSpacing);
  int y = std::clamp(textTop - height - padding * 2 - metrics.verticalSpacing, top,
                     std::max(top, safe.bottom() - height - padding * 2));
  // Clamping an above-text toolbar at the top edge can cover the handles.
  // Move below them when that happens, keeping both endpoints reachable.
  const auto overlaps = [&](const Rect rect) {
    return !rect.empty() && rect.x < x + width && rect.right() > x && rect.y < y + padding + height &&
           rect.bottom() > y + padding;
  };
  if (overlaps(layout.startHandle) || overlaps(layout.endHandle)) {
    const int below = std::max(layout.startHandle.bottom(), layout.endHandle.bottom()) + metrics.verticalSpacing;
    int firstBottom = safe.bottom();
    for (const auto handle : {layout.startHandle, layout.endHandle}) {
      if (!handle.empty()) firstBottom = std::min<int>(firstBottom, handle.bottom());
    }
    const int buttonY = below + height <= safe.bottom() ? below : firstBottom + metrics.verticalSpacing;
    y = std::clamp(buttonY - padding, top, std::max(top, safe.bottom() - height - padding));
  }
  for (int i = 0; i < actionCount; ++i) {
    layout.actions[i] = {static_cast<int16_t>(x + padding + i * (buttonWidth + padding)),
                         static_cast<int16_t>(y + padding), static_cast<int16_t>(buttonWidth),
                         static_cast<int16_t>(height)};
  }
}

Rect handle(const Rect safe, const Rect word, const bool left) {
  const int size = std::min<int>(std::min(safe.width, safe.height),
                                 std::max(24, UITheme::getInstance().getMetrics().verticalSpacing * 2));
  const int edge = left ? word.x : word.right();
  return {static_cast<int16_t>(std::clamp(edge - (left ? size : 0), static_cast<int>(safe.x), safe.right() - size)),
          static_cast<int16_t>(std::clamp<int>(word.bottom(), static_cast<int>(safe.y), safe.bottom() - size)),
          static_cast<int16_t>(size), static_cast<int16_t>(size)};
}

int actionAt(const Layout& layout, const int x, const int y) {
  for (int i = 0; i < actionCount; ++i) {
    if (layout.actions[i].contains(x, y)) return i;
  }
  return -1;
}

void draw(const GfxRenderer& renderer, const Layout& layout, const bool showActions, const bool lookupEnabled) {
  const auto spec = uiScaleSpec();
  // Fixed-capacity SDK frame: no retained widget tree or framebuffer allocation.
  fui::GfxRendererFrame<actionCount> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  const auto drawHandle = [&](Rect rect, bool left) {
    if (rect.empty()) return;
    const uint8_t corners = fui::CornersAll & ~(left ? fui::CornerTopRight : fui::CornerTopLeft);
    ui.target.fill(rect, fui::Paint::solid(fui::Color::Black), rect.width / 2, corners);
  };
  drawHandle(layout.startHandle, layout.startOnLeft);
  drawHandle(layout.endHandle, layout.endOnLeft);
  if (!showActions) return;
  const int actionRadius =
      SETTINGS.uiTheme == CrossPointSettings::LYRA ? 3 : UITheme::getInstance().getMetrics().optionPopupSelectionRadius;
  for (int i = 0; i < actionCount; ++i) {
    fui::ButtonProps props;
    props.label = label(i);
    props.text.font = fui::GfxRendererTarget::FONT_SMALL;
    props.text.maxLines = 2;
    props.styles = fui::outlinedButtonStyles(actionRadius);
    props.styles.disabled = props.styles.normal;
    props.styles.disabled.foreground = fui::Paint::dither(fui::Color::LightGray);
    props.enabled = i != 0 || lookupEnabled;
    // Use the same exact bounds for painting and hit testing, as upstream does.
    props.minTouchSize = 0;
    fui::button(ui.frame, layout.actions[i], props);
  }
}
}  // namespace TextSelectionOverlay
#endif
