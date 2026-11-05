#pragma once

#include <AppCapabilities.h>
#if CROSSDINK_APP_CAP_TOUCH
#include <FreeInkUICore.h>

class GfxRenderer;

// Reader-specific placement; the SDK owns button paint and rectangle hit testing.
namespace TextSelectionOverlay {
using Rect = freeink::ui::Rect;
constexpr int actionCount = 2;
struct Layout {
  Rect actions[actionCount];
  Rect startHandle;
  Rect endHandle;
  bool startOnLeft = true;
  bool endOnLeft = false;
};
Rect safeArea(const GfxRenderer& renderer);
void placeActions(const GfxRenderer& renderer, Layout& layout, int anchorX, int textTop);
Rect handle(Rect safe, Rect word, bool left);
int actionAt(const Layout& layout, int x, int y);
void draw(const GfxRenderer& renderer, const Layout& layout, bool showActions, bool lookupEnabled);
}  // namespace TextSelectionOverlay
#endif
