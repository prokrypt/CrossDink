#pragma once

#include <WiFi.h>

#include <algorithm>
#include <atomic>

inline bool hasActiveStationWifiConnection() {
  return (WiFi.getMode() & WIFI_MODE_STA) && WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
inline int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}

// Station driver up (STA_START..STA_STOP). With no link yet that is the association / DHCP phase.
inline std::atomic<bool>& wifiStaActive() {
  static std::atomic<bool> active{false};
  return active;
}

#ifndef SIMULATOR
inline void onWifiStaStart(WiFiEvent_t, WiFiEventInfo_t) { wifiStaActive().store(true, std::memory_order_relaxed); }
inline void onWifiStaStop(WiFiEvent_t, WiFiEventInfo_t) { wifiStaActive().store(false, std::memory_order_relaxed); }
#endif

// Call once at boot; the flag is only ever read by the header.
inline void registerWifiStaTracking() {
#ifndef SIMULATOR
  WiFi.onEvent(onWifiStaStart, ARDUINO_EVENT_WIFI_STA_START);
  WiFi.onEvent(onWifiStaStop, ARDUINO_EVENT_WIFI_STA_STOP);
#endif
}

// wifiHeaderBars() value while a station join is in progress.
constexpr int WIFI_HEADER_CONNECTING = 5;

// Header Wi-Fi level: 0 = no station link, WIFI_HEADER_CONNECTING = joining, else 1..4 bars
// (a live link shows at least one).
// The hysteresis state is shared by the renderer and the main-loop poll, so both see the same value.
inline int wifiHeaderBars() {
  if (!hasActiveStationWifiConnection()) {
    return wifiStaActive().load(std::memory_order_relaxed) ? WIFI_HEADER_CONNECTING : 0;
  }
  static int bars = 0;
  bars = barsForRssi(WiFi.RSSI(), bars);
  return std::max(bars, 1);
}
