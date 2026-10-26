#pragma once

#ifndef SIMULATOR
#include <mutex>

#include "esp_wifi.h"
#endif

// Keeps Wi-Fi power save off while any guard lives. Guards overlap across tasks
// (OPDS preloads run three fetches at once), so the first one saves the mode and
// the last one restores it: a save and restore per guard left PS_NONE behind
// whenever they ended out of order, and with it the wifi PM lock for the session.
class WifiPowerSaveGuard final {
 public:
#ifndef SIMULATOR
  WifiPowerSaveGuard() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_++ == 0) {
      restorePreviousMode_ = esp_wifi_get_ps(&previousMode_) == ESP_OK;
      esp_wifi_set_ps(WIFI_PS_NONE);
    }
  }

  ~WifiPowerSaveGuard() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (--count_ == 0 && restorePreviousMode_) {
      esp_wifi_set_ps(previousMode_);
    }
  }
#else
  WifiPowerSaveGuard() = default;
  ~WifiPowerSaveGuard() = default;
#endif

  WifiPowerSaveGuard(const WifiPowerSaveGuard&) = delete;
  WifiPowerSaveGuard& operator=(const WifiPowerSaveGuard&) = delete;

 private:
#ifndef SIMULATOR
  static inline std::mutex mutex_;
  static inline int count_ = 0;
  static inline wifi_ps_type_t previousMode_ = WIFI_PS_MIN_MODEM;
  static inline bool restorePreviousMode_ = false;
#endif
};
