#pragma once

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <InputManager.h>
#include <Knobs.h>
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
  volatile bool refreshLightSleep = false;
#if CONFIG_PM_ENABLE
  // Held while the device is active. Releasing it is what lets DFS drop to the
  // floor and lets tickless idle enter light sleep.
  esp_pm_lock_handle_t cpuFreqLock = nullptr;
  // Held only for the duration of an EPD busy-wait (see beginDisplayBusyWait),
  // so tickless idle can never light-sleep mid-refresh, unless refreshLightSleep.
  esp_pm_lock_handle_t displayPmLock = nullptr;
  // cpuFreqLock is wanted while the device is active, except during an EPD
  // busy-wait, where the CPU only waits on the panel and can idle at the DFS
  // floor. cpuFreqLockHeld tracks what has actually been acquired.
  bool cpuFreqLockHeld = false;
  bool displayBusyWaitActive = false;
  bool displayPmLockHeld = false;  // this busy-wait took displayPmLock
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
  mutable uint16_t _batteryCached256 = 0;        // I2C path: last read in 1/256 % (CW2017 fraction)

  // Set by a Wi-Fi screen that manages its own radio power (File Transfer in
  // STA mode): an active Wi-Fi link then no longer forces power saving off.
  bool radioIdleSleepAllowed = false;
  int backgroundWorkCount = 0;  // guarded by modeMutex

  // Live Lock instances. The render task, background work and deep-sleep prep
  // can overlap, so each keeps the full clock until the last one ends.
  uint8_t lockCount = 0;
  SemaphoreHandle_t modeMutex = nullptr;  // Protect access to lockCount and backgroundWorkCount

 public:
#if defined(BOARD_HAS_PSRAM)
  static constexpr int LOW_POWER_FREQ = 80;  // MHz
#else
  static constexpr int LOW_POWER_FREQ = 10;  // MHz
#endif
  // DFS floor when power management is enabled. 80 MHz keeps APB pinned at
  // 80 MHz across every mode, so SPI dividers computed at bus setup stay valid
  // no matter what the CPU clock is doing.
  static constexpr int DFS_MIN_FREQ = 80;                    // MHz
  static KNOB_ALIAS(IDLE_POWER_SAVING_MS, idlePowerSaveMs);  // ms, Goodies > Knobs
  static KNOB_ALIAS(BATTERY_POLL_MS, batteryPollMs);         // ms

  void begin();

  // Control CPU frequency for power saving. With power management enabled this
  // toggles the activity PM lock instead, so idle time light-sleeps.
  void setPowerSaving(bool enabled);

  // Registered with HalDisplay via EInkDisplay::setBusyWaitHooks() so the SDK's
  // BUSY-pin poll never light-sleeps mid-refresh. No-ops when PM is disabled or
  // lock creation failed.
  void beginDisplayBusyWait();
  void endDisplayBusyWait();
  // Busy-waits skip the no-light-sleep lock, and HalDisplay's slice hook blocks
  // the task until a BUSY level interrupt (also a light-sleep wake), so tickless
  // idle can light-sleep through the waveform. Set between refreshes.
  void setRefreshLightSleep(bool allowed) { refreshLightSleep = allowed; }
  bool refreshLightSleepAllowed() const { return refreshLightSleep; }

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

  // Counted, callable from any task: while any hold is open the CPU stays at
  // full speed (and so out of light sleep), and setPowerSaving(true) from the
  // main loop is ignored. For work that runs on its own task while the loop
  // idles: web transfers, background layout or decoding.
  void beginBackgroundWork();
  void endBackgroundWork();

  // Setup wake up GPIO and enter deep sleep
  // Should be called inside main loop() to handle the lockCount
  void startDeepSleep(HalGPIO& gpio) const;
  // The sleep-entry step in progress, for main.cpp's stuck-sleep guard log.
  static inline const char* volatile sleepStep = "";
  // Also wake from deep sleep when the charger STAT pin changes (charge start or
  // stop), so the battery log can record it. Off unless set.
  bool wakeOnChargeChange = false;

  // Get battery percentage (range 0-100)
  uint16_t getBatteryPercentage() const;
  // Same cached read in 1/256 %. Only the CW2017 gauge reports a fraction;
  // other backends return whole percents * 256.
  uint16_t getBatteryPercent256() const;

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
  // full performance. When the last Lock instance is destroyed (goes out of scope), power saving will be re-enabled.
  // Locks may overlap across tasks and nest.
  class Lock {
    friend class HalPowerManager;

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
