#pragma once

#include <cstdint>

// Small, allocation-free state for the temporary input lock.  Unsigned
// subtraction keeps the timeout correct across millis() wraparound.
class QuickLockState {
 public:
  bool toggle(uint32_t nowMs) {
    locked_ = !locked_;
    lockedAtMs_ = locked_ ? nowMs : 0U;
    return locked_;
  }

  bool isLocked() const { return locked_; }

  bool shouldSleep(uint32_t nowMs, uint32_t timeoutMs) const {
    return locked_ && timeoutMs != 0U && static_cast<uint32_t>(nowMs - lockedAtMs_) >= timeoutMs;
  }

  // ms until shouldSleep() turns true; UINT32_MAX when it never will.
  uint32_t msUntilSleep(uint32_t nowMs, uint32_t timeoutMs) const {
    if (!locked_ || timeoutMs == 0U) return UINT32_MAX;
    const uint32_t elapsed = nowMs - lockedAtMs_;
    return elapsed >= timeoutMs ? 0U : timeoutMs - elapsed;
  }

 private:
  bool locked_ = false;
  uint32_t lockedAtMs_ = 0U;
};
