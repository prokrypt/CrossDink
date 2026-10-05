#include "Toast.h"

#include <Arduino.h>
#include <GfxRenderer.h>

#include "fontIds.h"

namespace {
constexpr int PAD_X = 20;
constexpr int PAD_Y = 12;
unsigned long heldAt = 0;  // 0 = no hold
}  // namespace

namespace Toast {

Rect bounds(const GfxRenderer& renderer, const char* msg) {
  const int w = renderer.getTextWidth(UI_10_FONT_ID, msg) + PAD_X * 2;
  const int h = renderer.getLineHeight(UI_10_FONT_ID) + PAD_Y * 2;
  return Rect((renderer.getScreenWidth() - w) / 2, (renderer.getScreenHeight() - h) / 2, w, h);
}

void draw(const GfxRenderer& renderer, const char* msg) {
  const Rect r = bounds(renderer, msg);
  renderer.fillRect(r.x, r.y, r.width, r.height, true);
  renderer.drawRect(r.x, r.y, r.width, r.height, false);
  renderer.drawText(UI_10_FONT_ID, r.x + PAD_X, r.y + PAD_Y, msg, false);
}

void show(const GfxRenderer& renderer, const char* msg) {
  draw(renderer, msg);
  renderer.displayBuffer();
  heldAt = millis() | 1;
}

bool holding() {
  if (heldAt != 0 && millis() - heldAt >= DURATION_MS) heldAt = 0;
  return heldAt != 0;
}

void release() { heldAt = 0; }

}  // namespace Toast
