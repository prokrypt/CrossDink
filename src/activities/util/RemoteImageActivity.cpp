#include "RemoteImageActivity.h"

#include <HalGPIO.h>

#include <cstring>

void RemoteImageActivity::onEnter() {
  Activity::onEnter();
  planes = std::move(incoming);
  requestUpdate();
}

void RemoteImageActivity::onUserInput() {
  // Tilt activity also lands here; only keys, the Home key and the touchscreen close it.
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased()) inputSeen = true;
#if CROSSDINK_APP_CAP_TOUCH
  if (gpio.wasTouchActivity()) inputSeen = true;
#endif
}

void RemoteImageActivity::loop() {
  // A newer picture replaces this one in place.
  if (incoming) {
    {
      RenderLock lock;  // render() reads planes
      planes = std::move(incoming);
      drawn = false;
    }
    requestUpdate();
    return;
  }
  // Close once that input has fully ended, so neither its press nor its release
  // (nor the Home key's tap, eaten by the global route) reaches the screen below.
  if (!inputSeen) return;
  for (uint8_t button = HalGPIO::BTN_BACK; button <= HalGPIO::BTN_POWER; ++button) {
    if (gpio.isPressed(button)) return;
  }
#if CROSSDINK_APP_CAP_TOUCH
  float nx = 0.0f;
  float ny = 0.0f;
  if (gpio.isTouchHeldAt(nx, ny)) return;
#endif
  finish();
}

// Same sequence as BmpViewerActivity::showDecoded(): one direct-gray refresh, then the
// B/W baseline (white only at level 3) for the balanced exit paint.
void RemoteImageActivity::render(RenderLock&&) {
  if (drawn || !planes) return;
  drawn = true;
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
  const uint8_t* lsb = planes.get();
  const uint8_t* msb = lsb + planeBytes;
  uint8_t* frame = renderer.getFrameBuffer();
  if (renderer.supportsDirectGrayscale() && renderer.displayDirectGrayscaleBase()) {
    renderer.copyGrayscalePlanes(lsb, msb);
    renderer.displayGrayBuffer();
    for (size_t i = 0; i < planeBytes; i++) frame[i] = lsb[i] & msb[i];
    renderer.cleanupGrayscaleWithFrameBuffer();
    return;
  }
  // No direct gray: the MSB plane alone is the picture at 2 levels.
  LOG_INF("RIMG", "No direct gray, showing B/W");
  memcpy(frame, msb, planeBytes);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
