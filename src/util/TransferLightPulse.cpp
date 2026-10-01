#include "TransferLightPulse.h"

#include <Arduino.h>
#include <CrossDinkHalFrontlight.h>
#include <Logging.h>

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
  // Idle at the user's level (0 if off). Pulse band by level: up to 10% -> 0-10,
  // 11-25% -> 10-25, above 25% -> 10 to the level (knobs: floor 10, peak 25).
  basePercent = savedOn ? savedBrightness : 0;
  lowPercent = basePercent > kLitFloorPercent ? kLitFloorPercent : 0;
  highPercent = basePercent > kPeakPercent        ? basePercent
                : basePercent > kLitFloorPercent ? kPeakPercent
                                                 : kLitFloorPercent;
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
  LOG_DBG("LIGHT", "Transfer pulse stopped: back to the user's %u%% %s", active->savedBrightness,
          active->savedOn ? "on" : "off");
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
    // One cycle at constant speed: level -> high -> low -> level, so every
    // cycle starts and ends at the user's level.
    const uint32_t span = highPercent - lowPercent;
    const uint32_t up = highPercent - basePercent;
    const uint32_t d = (now - pulseStartMs) % kCycleMs * 2 * span / kCycleMs;
    target = static_cast<uint8_t>(d < up          ? basePercent + d
                                  : d < up + span ? highPercent - (d - up)
                                                  : lowPercent + (d - up - span));
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
  write(highPercent);
  LOG_DBG("LIGHT", "Transfer pulse held at %u%%", highPercent);
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
