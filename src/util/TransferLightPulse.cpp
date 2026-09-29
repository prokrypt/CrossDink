#include "TransferLightPulse.h"

#include <Arduino.h>
#include <CrossDinkHalFrontlight.h>
#include <Logging.h>

namespace {
constexpr uint32_t kCycleMs = 1000;
constexpr uint32_t kWriteIntervalMs = 20;
constexpr uint8_t kPeakPercent = 25;
}  // namespace

void TransferLightPulse::begin(const uint32_t holdForMs) {
  armed = false;
  entryHold = false;
  if (!Frontlight.present()) {
    return;
  }
  savedBrightness = Frontlight.brightness();
  savedOn = Frontlight.isOn();
  userOverride = false;
  pulsing = false;
  stopAtMs = 0;
  held = false;
  armed = true;
  if (holdForMs > 0 && savedOn && savedBrightness > 0) {
    // Write nothing: `written` still lets update() spot a user change.
    entryHold = true;
    holdStartMs = millis();
    holdMs = holdForMs;
    written = savedBrightness;
    LOG_DBG("LIGHT", "Transfer light hold %u%% for %lu ms", savedBrightness, static_cast<unsigned long>(holdMs));
    return;
  }
  write(0);
  Frontlight.setOn(true);
}

void TransferLightPulse::write(const uint8_t percent) {
  Frontlight.setBrightness(percent);
  written = percent;
  lastWriteMs = millis();
}

void TransferLightPulse::update(const bool transferActive) {
  if (!armed || userOverride || held) {
    return;
  }
  if (Frontlight.brightness() != written || !Frontlight.isOn()) {
    userOverride = true;
    LOG_DBG("LIGHT", "Transfer pulse stopped: user set %u%% %s", Frontlight.brightness(),
            Frontlight.isOn() ? "on" : "off");
    return;
  }

  const uint32_t now = millis();
  if (entryHold) {
    if (now - holdStartMs < holdMs) return;  // data during the hold is ignored
    entryHold = false;
    LOG_DBG("LIGHT", "Transfer light hold over after %lu ms (%s)", static_cast<unsigned long>(now - holdStartMs),
            transferActive ? "data" : "idle");
  }
  if (transferActive) {
    if (!pulsing) {
      pulsing = true;
      pulseStartMs = now;
      LOG_DBG("LIGHT", "Transfer pulse start");
    }
    stopAtMs = 0;  // data resumed while fading: keep the same waveform going
  } else if (pulsing) {
    if (stopAtMs == 0) {
      // Finish the current cycle so the light ramps down to 0.
      const uint32_t cycles = (now - pulseStartMs) / kCycleMs + 1;
      stopAtMs = pulseStartMs + cycles * kCycleMs;
    }
    if (static_cast<int32_t>(now - stopAtMs) >= 0) {
      pulsing = false;
      stopAtMs = 0;
      LOG_DBG("LIGHT", "Transfer pulse stop");
    }
  }

  uint8_t target = 0;
  if (pulsing) {
    const uint32_t phase = (now - pulseStartMs) % kCycleMs;
    const uint32_t half = kCycleMs / 2;
    const uint32_t ramp = phase < half ? phase : kCycleMs - phase;
    target = static_cast<uint8_t>(ramp * kPeakPercent / half);
  }
  if (target != written && (target == 0 || now - lastWriteMs >= kWriteIntervalMs)) {
    write(target);
  }
}

void TransferLightPulse::holdOn() {
  if (!armed || userOverride) {
    return;
  }
  held = true;
  entryHold = false;
  write(kPeakPercent);
  LOG_DBG("LIGHT", "Transfer pulse held at %u%%", kPeakPercent);
}

void TransferLightPulse::end() {
  if (!armed) {
    return;
  }
  armed = false;
  entryHold = false;
  if (userOverride) {
    return;
  }
  Frontlight.setBrightness(savedBrightness);
  Frontlight.setOn(savedOn);
  if (pulsing) {
    LOG_DBG("LIGHT", "Transfer pulse stop");
  }
}
