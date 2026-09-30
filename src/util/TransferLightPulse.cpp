#include "TransferLightPulse.h"

#include <Arduino.h>
#include <CrossDinkHalFrontlight.h>
#include <Logging.h>

#include <algorithm>

namespace {
KNOB_ALIAS(kCycleMs, pulseCycleMs);  // Goodies > Knobs; the peak's range stays above the floor's
uint32_t lastAnyWriteMs = 0;
KNOB_ALIAS(kPeakPercent, pulsePeakPct);
KNOB_ALIAS(kLitFloorPercent, pulseFloorPct);  // pulse floor when the light was already on
TransferLightPulse* active = nullptr;         // the armed pulse; one at a time
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
  active = this;
  // Idle at the user's level. A lit light never goes above it: each pulse dips
  // to the lit floor (half the level if that is lower) and back. Off pulses up.
  basePercent = savedOn ? savedBrightness : 0;
  swingPercent = basePercent == 0 ? kPeakPercent : std::min<uint8_t>(kLitFloorPercent, basePercent / 2);
  if (holdForMs > 0 && savedOn && savedBrightness > 0) {
    // Write nothing: `written` still lets update() spot a user change.
    entryHold = true;
    holdStartMs = millis();
    holdMs = holdForMs;
    written = savedBrightness;
    LOG_DBG("LIGHT", "Transfer light hold %u%% for %lu ms", savedBrightness, static_cast<unsigned long>(holdMs));
    return;
  }
  write(basePercent);
  Frontlight.setOn(true);
}

void TransferLightPulse::write(const uint8_t percent) {
  Frontlight.setBrightness(percent);
  written = percent;
  lastWriteMs = millis();
  lastAnyWriteMs = lastWriteMs;
}

void TransferLightPulse::yieldToUser() {
  if (!active || active->userOverride) return;
  active->userOverride = true;
  Frontlight.setBrightness(active->savedBrightness);
  Frontlight.setOn(active->savedOn);
  LOG_DBG("LIGHT", "Transfer pulse stopped: user slide from %u%%", active->savedBrightness);
}

bool TransferLightPulse::animating() { return millis() - lastAnyWriteMs < 5 * WRITE_INTERVAL_MS; }

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
      // Finish the current cycle so the light ramps back to the user's level.
      const uint32_t cycles = (now - pulseStartMs) / kCycleMs + 1;
      stopAtMs = pulseStartMs + cycles * kCycleMs;
    }
    if (static_cast<int32_t>(now - stopAtMs) >= 0) {
      pulsing = false;
      stopAtMs = 0;
      LOG_DBG("LIGHT", "Transfer pulse stop");
    }
  }

  uint8_t target = basePercent;
  if (pulsing) {
    const uint32_t phase = (now - pulseStartMs) % kCycleMs;
    const uint32_t half = kCycleMs / 2;
    const int32_t ramp = static_cast<int32_t>(phase < half ? phase : kCycleMs - phase);
    target = static_cast<uint8_t>(basePercent + ramp * (swingPercent - basePercent) / static_cast<int32_t>(half));
  }
  if (target != written && (target == basePercent || now - lastWriteMs >= WRITE_INTERVAL_MS)) {
    write(target);
  }
}

void TransferLightPulse::holdOn() {
  if (!armed || userOverride) {
    return;
  }
  held = true;
  entryHold = false;
  const uint8_t level = basePercent ? basePercent : kPeakPercent;  // never above a lit user level
  write(level);
  LOG_DBG("LIGHT", "Transfer pulse held at %u%%", level);
}

void TransferLightPulse::end() {
  if (!armed) {
    return;
  }
  armed = false;
  entryHold = false;
  if (active == this) active = nullptr;
  if (userOverride) {
    return;
  }
  Frontlight.setBrightness(savedBrightness);
  Frontlight.setOn(savedOn);
  if (pulsing) {
    LOG_DBG("LIGHT", "Transfer pulse stop");
  }
}
