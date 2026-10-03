#pragma once

#include <WiFi.h>

#include <algorithm>

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

// Header Wi-Fi level: 0 = no station link, else 1..4 bars (a live link shows at least one).
// The hysteresis state is shared by the renderer and the main-loop poll, so both see the same value.
inline int wifiHeaderBars() {
  if (!hasActiveStationWifiConnection()) return 0;
  static int bars = 0;
  bars = barsForRssi(WiFi.RSSI(), bars);
  return std::max(bars, 1);
}
