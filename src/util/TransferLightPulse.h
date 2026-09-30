#pragma once

#include <Knobs.h>

#include <cstdint>

// Pulses the frontlight while a Wi-Fi or USB Drive file transfer is moving
// data (1 s cycle) and holds the user's level when idle. A lit light dips from
// the user's level to 10% (or half the level, if lower) and back, never above
// it; a light that was off at begin() pulses from off to 25%. Every pulse runs
// to the end of its cycle, so even a short request gives one full blink and the
// light always ramps back to the user's level instead of cutting off. Saves the user's
// brightness/on state on begin() and restores it on end(). A brightness or
// on/off change the pulse did not make (swipe, frontlight panel) stops the
// pulse until end() and is left as the user set it. Main loop only; never
// writes SETTINGS. Inert on boards without a frontlight.
class TransferLightPulse {
 public:
  // How long after the last data a caller should still report activity.
  static KNOB_ALIAS(TAIL_MS, pulseTailMs);  // Goodies > Knobs
  static KNOB_ALIAS(WRITE_INTERVAL_MS, pulseWriteMs);
  // A pulse wrote within the last few steps: the main loop ticks at
  // WRITE_INTERVAL_MS meanwhile, else a 50-250 ms idle tick makes it step.
  static bool animating();
  // A user slide is starting: put back the level the pulse saved so the slide
  // starts from it, and stop the pulse so end() keeps what the user sets.
  static void yieldToUser();

  // A light that is on keeps its level for holdMs before pulsing starts (0 =
  // pulse at once). A user change during the hold is kept as usual.
  void begin(uint32_t holdMs = KNOBS.pulseHoldMs);
  void update(bool transferActive);
  // Stops the pulse and holds the light steady until end(): at the user's level
  // if it was lit, else at the pulse peak.
  void holdOn();
  void end();
  ~TransferLightPulse() { end(); }

 private:
  void write(uint8_t percent);

  uint32_t pulseStartMs = 0;
  uint32_t stopAtMs = 0;  // end of the cycle the pulse fades out on; 0 = running
  uint32_t lastWriteMs = 0;
  uint32_t holdStartMs = 0;
  uint32_t holdMs = 0;
  uint8_t savedBrightness = 0;
  uint8_t written = 0;
  uint8_t basePercent = 0;   // idle level: the user's brightness, 0 if off
  uint8_t swingPercent = 0;  // the other end of each pulse
  bool savedOn = false;
  bool armed = false;
  bool userOverride = false;
  bool pulsing = false;
  bool held = false;
  bool entryHold = false;
};
