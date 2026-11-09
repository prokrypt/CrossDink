#include "HalFrontlight.h"

#include <FrontlightGamma.h>
#include <Logging.h>
#include <PowerCounters.h>

#ifndef SIMULATOR
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
  idleDim = DIM_FULL;
  overlay = NO_OVERLAY;
  sleepDark = false;
  driven = NOT_DRIVEN;   // begin() resets the PWM
  if (lit) startFade();  // boot and wake fade in from dark
  apply();
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  lastBrightness = percent > 100 ? 100 : percent;
  idleDim = DIM_FULL;
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
  if (on == lit && idleDim == DIM_FULL && !overlayActive()) {
    return;
  }
  lit = on;
  idleDim = DIM_FULL;
  overlay = NO_OVERLAY;
  if (fade) {
    startFade();
  } else {
    stopFade();
  }
  apply();
}

void HalFrontlight::setIdleDimFine(const uint16_t level, const uint32_t rampMs) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  idleDim = level > DIM_FULL ? DIM_FULL : level;
  if (rampMs > 0 && !overlayActive()) startFade(rampMs);
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
  {
    std::lock_guard<std::recursive_mutex> guard(lock);
    if (sleepDark || driven == NOT_DRIVEN) return;
    sleepDark = true;  // also when dark now: a fade-in may still be at 0
    overlay = NO_OVERLAY;
    if (driven != 0) {
      startFade();
    } else {
      stopFade();
    }
    apply();
  }
#ifndef SIMULATOR
  // The timer steps the fade (it takes the lock, so wait unlocked); bounded in case it stalls.
  for (uint32_t waited = 0; fading() && waited < fadeMs + 4 * FADE_STEP_MS; waited += FADE_STEP_MS) {
    vTaskDelay(pdMS_TO_TICKS(FADE_STEP_MS));
  }
#endif
}

bool HalFrontlight::fading() {
  std::lock_guard<std::recursive_mutex> guard(lock);
  return fadeStartUs != 0;
}

void HalFrontlight::apply() {
  // In 1/256 % of full: no whole-% steps in a fade or a duck (the manager dithers the duty).
  int32_t target = overlayActive() ? overlay * 256
                   : sleepDark     ? 0
                                   : ((lit ? lastBrightness : 0) * int32_t{idleDim} * 256 + DIM_FULL / 2) / DIM_FULL;
#ifndef SIMULATOR
  if (fadeStartUs != 0) {
    // Linear in percent; the manager's gamma curve maps each step to duty.
    const int64_t elapsedUs = esp_timer_get_time() - fadeStartUs;
    const int64_t spanUs = static_cast<int64_t>(fadeMs) * 1000;
    if (elapsedUs < spanUs) {
      target = fadeFrom + static_cast<int32_t>((target - fadeFrom) * elapsedUs / spanUs);
    } else {
      fadeStartUs = 0;
    }
  }
#endif
  drive(static_cast<uint16_t>(target));
}

void HalFrontlight::drive(const uint16_t q8) {
  // Every writer lands here: an unchanged level writes no PWM and logs nothing.
  if (q8 == driven) return;
  driven = q8;
  manager.setBrightnessFine(q8);
  // LED draw follows the PWM duty (FrontlightManager's curve), not the %.
  if (manager.present())
    PowerCounters::lightDuty(static_cast<uint16_t>((FrontlightGamma::perceptualDuty16(q8, 1023) + 8) / 16));
}

void HalFrontlight::startFade(const uint32_t ms) {
#ifndef SIMULATOR
  fadeFrom = driven == NOT_DRIVEN ? 0 : driven;
  fadeMs = ms;
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
