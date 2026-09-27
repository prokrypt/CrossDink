#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Logging.h>
#include <Wire.h>
#include <freertos/semphr.h>
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif

#include <cassert>

#include "HalGPIO.h"

// Opt-in battery telemetry for diagnosing a miscalibrated fuel gauge. Off in
// every shipping environment; the `debug` PlatformIO env sets it to 1. Defined
// here because this is the lowest layer that consumes it.
#ifndef CROSSDINK_BATTERY_DIAG_LOG
#define CROSSDINK_BATTERY_DIAG_LOG 0
#endif

class HalPowerManager;
extern HalPowerManager powerManager;  // Singleton

class HalPowerManager {
  int normalFreq = 0;  // MHz
  bool isLowPower = false;
#if CONFIG_PM_ENABLE
  // Held while the device is active. Releasing it is what lets DFS drop to the
  // floor and lets tickless idle enter light sleep.
  esp_pm_lock_handle_t cpuFreqLock = nullptr;
  // Held only for the duration of an EPD busy-wait (see beginDisplayBusyWait),
  // so tickless idle can never light-sleep mid-refresh.
  esp_pm_lock_handle_t displayPmLock = nullptr;
  // cpuFreqLock is wanted while the device is active, except during an EPD
  // busy-wait, where the CPU only waits on the panel and can idle at the DFS
  // floor. cpuFreqLockHeld tracks what has actually been acquired.
  bool cpuFreqLockHeld = false;
  bool displayBusyWaitActive = false;
  void syncCpuFreqLock();
  // Held while USB Drive owns the USB-OTG PHY. TinyUSB cannot service the host
  // across a light-sleep window, and USJ_NO_AUTO_LS_ON_CONNECTION only watches
  // the Serial/JTAG controller, not OTG.
  esp_pm_lock_handle_t usbDrivePmLock = nullptr;
  bool usbDrivePmLockHeld = false;
#endif

  mutable int _batteryCachedPercent = 0;  // Last read battery percentage * 10 (0-1000); callers divide by 10 (ADC/X4
                                          // path only — I2C/X3 path stores 0-100 directly)
  mutable unsigned long _batteryLastPollMs = 0;  // Timestamp of last battery read in milliseconds

  // Set by a Wi-Fi screen that manages its own radio power (File Transfer in
  // STA mode): an active Wi-Fi link then no longer forces power saving off.
  bool radioIdleSleepAllowed = false;

  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to currentLockMode

 public:
#if defined(BOARD_HAS_PSRAM)
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  // DFS floor when power management is enabled. 80 MHz keeps APB pinned at
  // 80 MHz across every mode, so SPI dividers computed at bus setup stay valid
  // no matter what the CPU clock is doing.
  static constexpr int DFS_MIN_FREQ = 80;                     // MHz
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 250;  // ms
  static constexpr unsigned long BATTERY_POLL_MS = 6000;      // ms

  void begin();

  // Control CPU frequency for power saving. With power management enabled this
  // toggles the activity PM lock instead, so idle time light-sleeps.
  void setPowerSaving(bool enabled);

  // Registered with HalDisplay via EInkDisplay::setBusyWaitHooks() so the SDK's
  // BUSY-pin poll never light-sleeps mid-refresh. No-ops when PM is disabled or
  // lock creation failed.
  void beginDisplayBusyWait();
  void endDisplayBusyWait();

  // Keeps light sleep off while a refresh runs in the background and the
  // render task goes on working, as the deferred menu refresh does. Unlike the
  // busy-wait hooks it leaves the CPU clock alone, since renders can run in
  // the meantime.
  void beginDisplayRefreshHold();
  void endDisplayRefreshHold();

  // Keeps the device out of light sleep while USB Drive is exposing the SD card
  // over USB-OTG. Idempotent, so repeated end calls on exit paths are safe.
  void setUsbDriveActive(bool active);

  // Lets setPowerSaving(true) take effect while Wi-Fi is up. The caller owns
  // keeping the CPU and modem at full power while it moves data.
  void setRadioIdleSleepAllowed(bool allowed) { radioIdleSleepAllowed = allowed; }

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the currentLockMode
  void startDeepSleep(HalGPIO& gpio) const;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;

#if CROSSDINK_BATTERY_DIAG_LOG
  // Raw battery telemetry for the diagnostic log, kept behind the flag so
  // shipping builds carry neither the struct nor the extra I2C traffic.
  struct BatteryDiagnostics {
    uint16_t soc = 0;         // 0-100, as reported by the backend
    uint16_t millivolts = 0;  // cell voltage; gauge boards report it directly
    bool charging = false;
    // Each field can fail independently, so a valid zero stays distinguishable
    // from an unread one.
    bool socKnown = false;
    bool millivoltsKnown = false;
    bool chargingKnown = false;
  };

  // Samples the battery backend directly, bypassing getBatteryPercentage()'s
  // poll cache and smoothing so every row is a fresh read. Returns false when
  // the active board reports no battery telemetry at all.
  bool getBatteryDiagnostics(BatteryDiagnostics& out) const;
#endif

  // RAII helper class to manage power saving locks
  // Usage: create an instance of Lock in a scope to disable power saving, for example when running a task that needs
  // full performance. When the Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();

    // Non-copyable and non-movable
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
