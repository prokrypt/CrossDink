#pragma once
#include <string>
#include <utility>

#include "activities/Activity.h"

class Bitmap;
class HalFile;

class SleepActivity final : public Activity {
 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool canSnapshotOverlayBackground,
                         std::string currentBookPath = {}, bool fromTimeout = false,
                         GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait)
      : Activity("Sleep", renderer, mappedInput),
        canSnapshotOverlayBackground(canSnapshotOverlayBackground),
        currentBookPath(std::move(currentBookPath)),
        fromTimeout(fromTimeout),
        sleepPopupOrientation(sleepPopupOrientation) {}
  void onEnter() override;
  // True when this sleep screen needs nothing the outgoing activity saves on
  // exit, so it may be drawn before that activity's onExit() runs.
  static bool rendersBeforeExit(const std::string& currentBookPath, bool fromTimeout);

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderReadingStatsSleepScreen() const;
  void renderMinimalSleepScreen() const;
  void renderMinimalStatsSleepScreen() const;
  void renderDashboardSleepScreen() const;
  bool renderBitmapSleepScreen(Bitmap& bitmap, HalFile& file) const;
  void renderLastScreenSleepScreen() const;
  void renderBlankSleepScreen() const;
  void renderOverlaySleepScreen() const;
  void idlePanel() const;
  // Set while the pre-draw panel idle is left to the Direct gray image path,
  // which overlaps it with the decode.
  mutable bool panelIdlePending = false;
  bool canSnapshotOverlayBackground = false;
  bool overlayBackgroundBufferStored = false;
  std::string currentBookPath;
  bool fromTimeout = false;
  GfxRenderer::Orientation sleepPopupOrientation = GfxRenderer::Orientation::Portrait;
};
