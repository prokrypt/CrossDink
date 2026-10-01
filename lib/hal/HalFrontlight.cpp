#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight HalFrontlight::instance;

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  if (!manager.present()) {
    return;
  }
  manager.begin();
  lastBrightness = brightness > 100 ? 100 : brightness;
  manager.setColorTemperature(warmth > 100 ? 100 : warmth);
  lit = on;
  idleDim = 100;
  driven = 0xFF;  // begin() resets the PWM
  drive(lit ? lastBrightness : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", lastBrightness, manager.colorTemperature(), lit ? "on" : "off");
}

void HalFrontlight::setBrightness(const uint8_t percent) {
  lastBrightness = percent > 100 ? 100 : percent;
  idleDim = 100;
  if (lit) {
    drive(lastBrightness);
  }
}

void HalFrontlight::setWarmth(const uint8_t warmPercent) {
  manager.setColorTemperature(warmPercent > 100 ? 100 : warmPercent);
}

void HalFrontlight::setOn(const bool on) {
  if (on == lit && idleDim == 100) {
    return;
  }
  lit = on;
  idleDim = 100;
  drive(lit ? lastBrightness : 0);
}

void HalFrontlight::setIdleDim(const uint8_t percent) {
  idleDim = percent > 100 ? 100 : percent;
  if (lit) {
    // Rounded: at a low brightness the few duty steps fall mid-fade, not at its start and end.
    drive(static_cast<uint8_t>((lastBrightness * idleDim + 50) / 100));
  }
}

void HalFrontlight::drive(const uint8_t percent) {
  // Every writer lands here: an unchanged level writes no PWM and logs nothing.
  if (percent == driven) return;
  driven = percent;
  manager.setBrightness(percent);
}

void HalFrontlight::prepareForDeepSleep() {
#ifdef FREEINK_FRONTLIGHT_LS
  manager.park();
#endif
}

void HalFrontlight::releaseAfterWake() {
#ifdef FREEINK_FRONTLIGHT_LS
  manager.releaseOnWake();
#endif
}
