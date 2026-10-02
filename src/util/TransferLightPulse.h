#pragma once

#include <Knobs.h>

#include <cstdint>

// Pulses the frontlight while a Wi-Fi or USB Drive file transfer is moving
// data (1 s cycle) and holds the user's level when idle (off counts as 0%).
// The band depends on that level: up to 10% pulses 0-10%, 11-25% pulses
// 10-25%, above 25% pulses between 10% and the level. Every pulse runs to the
// end of its cycle, so even a short request gives one full blink and the light
// always ramps back to the user's level instead of cutting off. Drives the
// light through the HAL overlay only, so the user's brightness/on state (the
// pulldown, gestures, SETTINGS) never sees it; end() drops the overlay. A user
// brightness or on/off change (swipe, pulldown) ends the overlay, which stops
// the pulse until end(). Main loop only. Inert on boards without a frontlight.
class TransferLightPulse {
 public:
  // How long after the last data a caller should still report activity.
  static KNOB_ALIAS(TAIL_MS, pulseTailMs);  // Goodies > Knobs
  static KNOB_ALIAS(WRITE_INTERVAL_MS, pulseWriteMs);
  // A pulse wrote within the last few steps: the main loop ticks at
  // WRITE_INTERVAL_MS meanwhile, else a 50-250 ms idle tick makes it step.
  static bool animating();
  // A user slide or a silent restart: drop the overlay (the user's level shows)
  // and stop the pulse until end().
  static void yieldToUser();

  // A light that is on keeps its level for holdMs before pulsing starts (0 =
  // pulse at once). A user change during the hold is kept as usual.
  void begin(uint32_t holdMs = KNOBS.pulseHoldMs);
  void update(bool transferActive);
  // Stops the pulse and holds the light steady at the top of its band until end().
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
  uint8_t written = 0;
  uint8_t basePercent = 0;   // idle level: the user's brightness, 0 if off
  uint8_t lowPercent = 0;    // pulse band
  uint8_t highPercent = 0;
  bool armed = false;
  bool userOverride = false;
  bool pulsing = false;
  bool held = false;
  bool entryHold = false;
};
