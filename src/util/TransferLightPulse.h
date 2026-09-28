#pragma once

#include <cstdint>

// Pulses the frontlight 0 -> 25% -> 0 (500 ms cycle) while a Wi-Fi file
// transfer is moving data, and holds it at 0% when idle. Saves the user's
// brightness/on state on begin() and restores it on end(). A brightness or
// on/off change the pulse did not make (swipe, frontlight panel) stops the
// pulse until end() and is left as the user set it. Main loop only; never
// writes SETTINGS. Inert on boards without a frontlight.
class TransferLightPulse {
 public:
  void begin();
  void update(bool transferActive);
  // Stops the pulse and holds the light steady at the pulse peak until end().
  void holdOn();
  void end();

 private:
  void write(uint8_t percent);

  uint32_t pulseStartMs = 0;
  uint32_t lastWriteMs = 0;
  uint8_t savedBrightness = 0;
  uint8_t written = 0;
  bool savedOn = false;
  bool armed = false;
  bool userOverride = false;
  bool pulsing = false;
  bool held = false;
};
