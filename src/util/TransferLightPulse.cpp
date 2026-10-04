#include "TransferLightPulse.h"

#include <Arduino.h>
#include <CrossDinkHalFrontlight.h>
#include <Logging.h>

namespace {
KNOB_ALIAS(kCycleMs, pulseCycleMs);  // Goodies > Knobs
uint32_t lastAnyWriteMs = 0;
KNOB_ALIAS(kOffTopPercent, pulseOffTopPct);  // pulse top when the light is off
TransferLightPulse* active = nullptr;        // the armed pulse; one at a time
}  // namespace

void TransferLightPulse::begin(const uint32_t holdForMs) {
  armed = false;
  entryHold = false;
  if (!Frontlight.present()) {
    return;
  }
  userOverride = false;
  pulsing = false;
  stopAtMs = 0;
  held = false;
  armed = true;
  active = this;
  // Idle at the user's level (0 if off or faded out by Light Timeout). Pulse band
  // from the level: see pulseLow()/pulseHigh().
  basePercent = Frontlight.isOn() && Frontlight.idleDimPercent() > 0 ? Frontlight.brightness() : 0;
  // An overlay at the level already shown: no PWM write, and the user's
  // brightness/on state (pulldown, SETTINGS) never sees the pulse.
  Frontlight.setOverlay(basePercent);
  written = basePercent;
  if (holdForMs > 0 && basePercent > 0) {
    entryHold = true;
    holdStartMs = millis();
    holdMs = holdForMs;
    LOG_DBG("LIGHT", "Transfer light hold %u%% for %lu ms", basePercent,
            static_cast<unsigned long>(holdMs == KNOB_HOLD ? KNOBS.pulseHoldMs : holdMs));
  }
}

uint8_t TransferLightPulse::lowPercent() const { return pulseLow(basePercent); }

uint8_t TransferLightPulse::highPercent() const { return pulseHigh(basePercent, kOffTopPercent); }

void TransferLightPulse::write(const uint8_t percent) {
  Frontlight.setOverlay(percent);
  written = percent;
  lastWriteMs = millis();
  lastAnyWriteMs = lastWriteMs;
}

void TransferLightPulse::yieldToUser() {
  if (!active || active->userOverride) return;
  active->userOverride = true;
  Frontlight.setOverlay(HalFrontlight::NO_OVERLAY);
  LOG_DBG("LIGHT", "Transfer pulse stopped: back to the user's %u%% %s", Frontlight.brightness(),
          Frontlight.isOn() ? "on" : "off");
}

bool TransferLightPulse::animating() { return millis() - lastAnyWriteMs < 5 * WRITE_INTERVAL_MS; }

void TransferLightPulse::update(const bool transferActive) {
  if (!armed || userOverride || held) {
    return;
  }
  if (!Frontlight.overlayActive()) {  // a user setBrightness()/setOn() ended it
    userOverride = true;
    LOG_DBG("LIGHT", "Transfer pulse stopped: user set %u%% %s", Frontlight.brightness(),
            Frontlight.isOn() ? "on" : "off");
    return;
  }

  const uint32_t now = millis();
  if (entryHold) {
    // Data during the hold is ignored.
    if (now - holdStartMs < (holdMs == KNOB_HOLD ? KNOBS.pulseHoldMs : holdMs)) return;
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
    const uint8_t low = lowPercent();
    const uint8_t high = highPercent();
    const uint32_t span = high - low;
    const uint32_t up = high - basePercent;
    const uint32_t d = (now - pulseStartMs) % kCycleMs * 2 * span / kCycleMs;
    target = static_cast<uint8_t>(d < up ? basePercent + d : d < up + span ? high - (d - up) : low + (d - up - span));
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
  write(highPercent());
  LOG_DBG("LIGHT", "Transfer pulse held at %u%%", highPercent());
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
  Frontlight.setOverlay(HalFrontlight::NO_OVERLAY);
  if (pulsing) {
    LOG_DBG("LIGHT", "Transfer pulse stop");
  }
}
