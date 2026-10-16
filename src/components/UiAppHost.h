#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>

#include "UiAppHelpers.h"

class GfxRenderer;
class MappedInputManager;

// FreeInkApp capacities shared by every screen: each distinct <interactions, handlers> pair compiles its own
// copy of the FreeInkUI widgets (~5 KB of flash for list() alone), while a bigger table costs only
// 32 B per interaction slot in the activity object. Pick the smallest bucket that fits; do not add sizes.
constexpr size_t kUiSmallInteractions = 12, kUiSmallHandlers = 4;
constexpr size_t kUiListInteractions = 24, kUiListHandlers = 7;
constexpr size_t kUiLargeInteractions = 48, kUiLargeHandlers = 11;
using UiAppSmall = freeink::ui::FreeInkApp<kUiSmallInteractions, kUiSmallHandlers>;
using UiAppList = freeink::ui::FreeInkApp<kUiListInteractions, kUiListHandlers>;
using UiAppLarge = freeink::ui::FreeInkApp<kUiLargeInteractions, kUiLargeHandlers>;

// Owns the small protocol shared by FreeInkUI activities.
template <size_t MaxInteractions, size_t MaxHandlers>
class UiAppHost {
 public:
  using App = freeink::ui::FreeInkApp<MaxInteractions, MaxHandlers>;
  using Screen = typename App::ScreenType;

  explicit UiAppHost(const GfxRenderer& renderer)
      : target(makeUiTarget(renderer)), app(target, target.deviceContext()) {}

  void reset() {
    ready.store(false, std::memory_order_release);
    applySharedUiTheme(app, target);
  }

  void render() {
    app.setDevice(target.deviceContext());
    app.render();
    ready.store(true, std::memory_order_release);
  }

  bool routeTouch(const MappedInputManager& input, freeink::ui::ActionEvent& event) {
    if (!ready.load(std::memory_order_acquire)) return false;
    const auto snapshot = touchSnapshotFrom(input);
    if (!snapshot.touchPressed && !snapshot.touchReleased && !snapshot.touchHeld) return false;
    event = app.route(snapshot);
    return true;
  }

  void closeRouting() { ready.store(false, std::memory_order_release); }
  bool routingReady() const { return ready.load(std::memory_order_acquire); }

  // The target is declared before the app because FreeInkApp keeps a reference
  // to it. Both are allocation-free and live for the owning Activity lifetime.
  freeink::ui::GfxRendererTarget target;
  App app;

 private:
  std::atomic<bool> ready{false};
};
