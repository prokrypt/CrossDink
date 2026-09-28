#pragma once

#include <cstdint>

// Pulses the frontlight 0 -> 25% -> 0 (1 s cycle) while a Wi-Fi file
// transfer is moving data, and holds it at 0% when idle. Every pulse runs to
// the end of its cycle, so even a short request gives one full blink and the
// light always ramps down to 0 instead of cutting off. Saves the user's
// brightness/on state on begin() and restores it on end(). A brightness or
// on/off change the pulse did not make (swipe, frontlight panel) stops the
// pulse until end() and is left as the user set it. Main loop only; never
// writes SETTINGS. Inert on boards without a frontlight.
class TransferLightPulse {
 public:
  // How long after the last data a caller should still report activity.
  static constexpr unsigned long TAIL_MS = 250;

  void begin();
  void update(bool transferActive);
  // Stops the pulse and holds the light steady at the pulse peak until end().
  void holdOn();
  void end();

 private:
  void write(uint8_t percent);

  uint32_t pulseStartMs = 0;
  uint32_t stopAtMs = 0;  // end of the cycle the pulse fades out on; 0 = running
  uint32_t lastWriteMs = 0;
  uint8_t savedBrightness = 0;
  uint8_t written = 0;
  bool savedOn = false;
  bool armed = false;
  bool userOverride = false;
  bool pulsing = false;
  bool held = false;
};
