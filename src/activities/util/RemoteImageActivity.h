#pragma once

#include <Memory.h>

#include "activities/Activity.h"

// Wi-Fi remote picture (POST /api/image, docs/serial-remote.md): shows a host-converted
// 4-level frame with one direct-gray refresh, then waits. The next touch or button
// closes it and is eaten; the screen below redraws.
class RemoteImageActivity final : public Activity {
 public:
  // The two physical gray planes, LSB plane then MSB plane, each display.getBufferSize()
  // bytes, panel-native orientation, 1 = white bit (level 0 black .. 3 white). The
  // server task fills `upload`; CMD:IMAGE (main task, server task waiting) moves it to
  // `incoming`, which only the main task touches.
  static inline HeapByteBuffer upload;
  static inline HeapByteBuffer incoming;

  RemoteImageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("RemoteImage", renderer, mappedInput) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onUserInput() override;
  bool blocksGlobalInput() const override { return true; }
  // Idle while shown: booster off after the draw, CPU light-sleeps between ticks
  // (with the Wi-Fi remote up too), no auto-sleep so the picture stays until input.
  bool powerOffPanelWhenIdle() const override { return true; }
  bool allowsRadioIdleSleep() override { return true; }
  bool preventAutoSleep() override { return true; }

 private:
  HeapByteBuffer planes;
  bool drawn = false;  // under the render lock: later repaints (battery, USB) keep the picture
  bool inputSeen = false;
};
