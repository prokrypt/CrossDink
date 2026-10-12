#pragma once

#include <FrontlightManager.h>

#include <cstdint>
#include <mutex>

// Thin HAL over the SDK's FrontlightManager (activities never touch SDK
// classes directly). Inert on boards without a frontlight, so it is always
// safe to call. State (brightness / warmth / on) is owned by the SDK manager;
// persistence policy lives in CrossPointSettings and the frontlight panel.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  // Bring up the PWM channel(s) and apply the given persisted state.
  void begin(uint8_t brightness, uint8_t warmth, bool on);

  bool present() const { return manager.present(); }
  bool hasColorTemperature() const { return manager.hasColorTemperature(); }

  // Brightness as 0-100 (total light on a warm/cool pair). Setting a value
  // while off leaves the light off; it is applied on the next on().
  void setBrightness(uint8_t percent);
  // Warm/cool mix, 0 = cool .. 100 = warm. No-op on single-channel boards.
  void setWarmth(uint8_t warmPercent);
  // fade: ramp to the new state over FADE_MS (a light toggle). Stepped by a timer,
  // never a delay in the caller; any other change mid-fade retargets or ends it.
  void setOn(bool on, bool fade = false);
  // Light timeout: drive the LEDs at `percent` of the user level without
  // changing brightness()/isOn() or settings. setBrightness()/setOn() end it.
  void setIdleDim(uint8_t percent);
  uint8_t idleDimPercent() const { return idleDim; }
  // Transfer throb: drive the LEDs at `percent` in place of the user level
  // without changing brightness()/isOn() or settings; NO_OVERLAY ends it. The
  // dim (Light Timeout, flash duck) does not show under it and is kept for after.
  // setBrightness()/setOn() end it.
  static constexpr uint8_t NO_OVERLAY = 0xFF;
  void setOverlay(uint8_t percent);
  bool overlayActive() const { return overlay != NO_OVERLAY; }
  // Sleep entry: fade the LEDs out without changing isOn() (the wake restores the
  // light and fades it in). Non-blocking; prepareForDeepSleep() cuts it short.
  void fadeOutForSleep();
  void prepareForDeepSleep();
  void releaseAfterWake();
  // Prints the settled duty after a ramp (see FrontlightManager::flushLog). Main loop.
  void flushLog() { manager.flushLog(); }

  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return manager.colorTemperature(); }
  bool isOn() const { return lit; }

 private:
  HalFrontlight() = default;
  static constexpr uint32_t FADE_MS = 300;
  static constexpr uint32_t FADE_STEP_MS = 20;
  void apply();
  void drive(uint8_t percent);
  void startFade();
  void stopFade();
  void stepFade();

  FrontlightManager manager;
  // The SDK manager folds "off" into brightness 0; keep the user's level and
  // the on/off state separate so toggling restores the previous level.
  uint8_t lastBrightness = 60;
  bool lit = false;
  uint8_t idleDim = 100;
  uint8_t overlay = NO_OVERLAY;
  uint8_t driven = 0xFF;   // the level last sent to the manager (0xFF: none yet)
  bool sleepDark = false;  // fadeOutForSleep(): drive 0, keep lit
  uint8_t fadeFrom = 0;
  int64_t fadeStartUs = 0;    // 0: no fade
  void* fadeTimer = nullptr;  // esp_timer_handle_t (device only)
  bool fadeTicking = false;
  // The fade timer and the flash duck's timer drive from the esp_timer task while the main loop sets state.
  std::recursive_mutex lock;

  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
