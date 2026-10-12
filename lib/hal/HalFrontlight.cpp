#include "HalFrontlight.h"

#include <FrontlightGamma.h>
#include <Logging.h>
#include <PowerCounters.h>

#ifndef SIMULATOR
#include <esp_timer.h>
#endif

HalFrontlight HalFrontlight::instance;

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  if (!manager.present()) {
    return;
  }
  std::lock_guard<std::recursive_mutex> guard(lock);
  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  idleDim = 100;
  overlay = NO_OVERLAY;
  sleepDark = false;
  driven = 0xFF;         // begin() resets the PWM
  if (lit) startFade();  // boot and wake fade in from dark
  apply();
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  lastBrightness = percent > 100 ? 100 : percent;
  idleDim = 100;
  overlay = NO_OVERLAY;
  stopFade();  // a slider or swipe follows the finger
  apply();
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on, const bool fade) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (on == lit && idleDim == 100 && !overlayActive()) {
    return;
  }
  lit = on;
  idleDim = 100;
  overlay = NO_OVERLAY;
  if (fade) {
    startFade();
  } else {
    stopFade();
  }
  apply();
}

void HalFrontlight::setIdleDim(const uint8_t percent) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  idleDim = percent > 100 ? 100 : percent;
  apply();  // a running fade retargets to the new level
}

void HalFrontlight::setOverlay(const uint8_t percent) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (percent == overlay) return;  // a blink's end with no overlay must not cut a fade
  overlay = percent;
  stopFade();  // the pulse owns the light
  apply();
}

void HalFrontlight::fadeOutForSleep() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (sleepDark || driven == 0xFF) return;
  sleepDark = true;  // also when dark now: a fade-in may still be at 0
  overlay = NO_OVERLAY;
  if (driven != 0) {
    startFade();
  } else {
    stopFade();
  }
  apply();
}

void HalFrontlight::apply() {
  // Rounded: at a low brightness the few duty steps fall mid-fade, not at its start and end.
  int target = overlayActive() ? overlay : sleepDark ? 0 : ((lit ? lastBrightness : 0) * idleDim + 50) / 100;
#ifndef SIMULATOR
  if (fadeStartUs != 0) {
    // Linear in percent; the manager's gamma curve maps each step to duty.
    const int64_t elapsedMs = (esp_timer_get_time() - fadeStartUs) / 1000;
    if (elapsedMs < static_cast<int64_t>(FADE_MS)) {
      target = fadeFrom + (target - fadeFrom) * static_cast<int>(elapsedMs) / static_cast<int>(FADE_MS);
    } else {
      fadeStartUs = 0;
    }
  }
#endif
  drive(static_cast<uint8_t>(target));
}

void HalFrontlight::drive(const uint8_t percent) {
  // Every writer lands here: an unchanged level writes no PWM and logs nothing.
  if (percent == driven) return;
  driven = percent;
  manager.setBrightness(percent);
  // LED draw follows the PWM duty (FrontlightManager's curve), not the %.
  if (manager.present())
    PowerCounters::lightDuty(static_cast<uint16_t>(FrontlightGamma::perceptualDuty(percent, 1023)));
}

void HalFrontlight::startFade() {
#ifndef SIMULATOR
  fadeFrom = driven == 0xFF ? 0 : driven;
  if (fadeTimer == nullptr) {
    const esp_timer_create_args_t args = {.callback = [](void*) { instance.stepFade(); },
                                          .arg = nullptr,
                                          .dispatch_method = ESP_TIMER_TASK,
                                          .name = "lightFade",
                                          .skip_unhandled_events = true};
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK) {
      LOG_ERR("LIGHT", "Fade timer not created, light snaps");
      return;
    }
    fadeTimer = timer;
  }
  fadeStartUs = esp_timer_get_time();
  if (!fadeTicking) {
    fadeTicking =
        esp_timer_start_periodic(static_cast<esp_timer_handle_t>(fadeTimer), FADE_STEP_MS * 1000ULL) == ESP_OK;
    if (!fadeTicking) fadeStartUs = 0;  // snap rather than stall mid-fade
  }
#endif
}

void HalFrontlight::stopFade() {
  fadeStartUs = 0;
#ifndef SIMULATOR
  if (fadeTicking) {
    esp_timer_stop(static_cast<esp_timer_handle_t>(fadeTimer));
    fadeTicking = false;
  }
#endif
}

void HalFrontlight::stepFade() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  apply();
  if (fadeStartUs == 0) stopFade();
}

void HalFrontlight::prepareForDeepSleep() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  stopFade();
  if (sleepDark) drive(0);  // cut a sleep fade short
#ifdef FREEINK_FRONTLIGHT_LS
  manager.park();
#endif
}

void HalFrontlight::releaseAfterWake() {
#ifdef FREEINK_FRONTLIGHT_LS
  manager.releaseOnWake();
#endif
}
