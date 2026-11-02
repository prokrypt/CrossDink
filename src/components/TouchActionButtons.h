#pragma once

#include <cstddef>
#include <cstdint>

#include "themes/BaseTheme.h"

class GfxRenderer;

// Fixed-capacity layout and drawing helpers for touch-only action groups. The
// same Layout must be used by rendering and hit testing so translated labels
// cannot make the two paths drift apart.
namespace TouchActionButtons {

constexpr size_t kMaxButtons = 4;
constexpr int kDefaultHeight = 56;
constexpr int kDefaultGap = 16;

struct Layout {
  Rect container;
  Rect buttons[kMaxButtons];
  uint8_t count = 0;

  Layout() : container(), buttons{Rect(), Rect(), Rect(), Rect()} {}

  const Rect* data() const { return buttons; }
};

Layout vertical(Rect container, uint8_t count, int buttonHeight = kDefaultHeight, int gap = kDefaultGap);

// Hit test with an optional finger-placement margin; callers should keep it
// within half the gap between buttons. Header-only so the unit test needs no
// renderer or theme stubs.
inline int indexAt(const Layout& layout, const int x, const int y, const int touchPadding = 0) {
  const int padding = touchPadding > 0 ? touchPadding : 0;
  for (uint8_t i = 0; i < layout.count; ++i) {
    const Rect& button = layout.buttons[i];
    if (button.width <= 0 || button.height <= 0) continue;
    if (x >= button.x - padding && x < button.x + button.width + padding && y >= button.y - padding &&
        y < button.y + button.height + padding) {
      return i;
    }
  }
  return -1;
}

void draw(const GfxRenderer& renderer, const Layout& layout, const char* const* labels, int primaryIndex = -1,
          int selectedIndex = -1, int fontId = 0);

}  // namespace TouchActionButtons
