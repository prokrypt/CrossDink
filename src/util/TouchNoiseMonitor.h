#pragma once

#include <cstdint>

// Flags runs of touch INT wakes that form no gesture: the signature of panel
// noise, e.g. a charging phone next to the device (20261003T154012Z: 192 wakes
// and nothing recognized in 10 s). A tap costs ~5-10 wakes and a swipe ~10-15,
// and either one clears its window. Log only; input is left alone.
struct TouchNoiseMonitor {
  static constexpr uint32_t WINDOW_MS = 5000;
  static constexpr uint32_t MIN_WAKES = 50;  // per WINDOW_MS, scaled for longer windows
  static constexpr uint32_t LOG_GAP_MS = 60000;

  // The closed window's counts, valid after update() returns true.
  uint32_t wakes = 0;
  uint32_t contacts = 0;
  uint32_t spanMs = 0;

  // Once per loop pass: the running touch wake total, whether a gesture was
  // recognized and whether a contact began this pass. True when a window just
  // closed as noise and no noise line went out in the last LOG_GAP_MS.
  bool update(const uint32_t nowMs, const uint32_t wakeTotal, const bool gesture, const bool contact) {
    if (!started_) {
      started_ = true;
      startMs_ = nowMs;
      startWakes_ = wakeTotal;
    }
    sawGesture_ = sawGesture_ || gesture;
    contacts_ += contact ? 1 : 0;
    const uint32_t span = nowMs - startMs_;
    if (span < WINDOW_MS) return false;
    const uint32_t n = wakeTotal - startWakes_;
    const bool noisy = !sawGesture_ &&
                       static_cast<uint64_t>(n) * WINDOW_MS >= static_cast<uint64_t>(MIN_WAKES) * span &&
                       (!logged_ || nowMs - lastLogMs_ >= LOG_GAP_MS);
    if (noisy) {
      logged_ = true;
      lastLogMs_ = nowMs;
      wakes = n;
      contacts = contacts_;
      spanMs = span;
    }
    startMs_ = nowMs;
    startWakes_ = wakeTotal;
    sawGesture_ = false;
    contacts_ = 0;
    return noisy;
  }

 private:
  bool started_ = false;
  bool sawGesture_ = false;
  bool logged_ = false;
  uint32_t startMs_ = 0;
  uint32_t startWakes_ = 0;
  uint32_t contacts_ = 0;
  uint32_t lastLogMs_ = 0;
};
