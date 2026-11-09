#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>

namespace WifiTransmitPower {
// ESP-IDF units are quarter-dBm; zero leaves the driver's default unchanged.
inline constexpr uint8_t TRANSMIT_POWER_LEVELS[] = {0, 60, 34, 20, 8};

namespace detail {
inline std::atomic<uint8_t> transmitPowerLimit{0};
}

inline void setLimit(uint8_t power) {
  const bool supported = std::any_of(std::begin(TRANSMIT_POWER_LEVELS), std::end(TRANSMIT_POWER_LEVELS),
                                     [power](const uint8_t level) { return level == power; });
  detail::transmitPowerLimit.store(supported ? power : 0, std::memory_order_relaxed);
}

inline uint8_t getLimit() { return detail::transmitPowerLimit.load(std::memory_order_relaxed); }
}  // namespace WifiTransmitPower
