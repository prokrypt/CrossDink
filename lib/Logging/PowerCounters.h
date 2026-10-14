#pragma once

#include <cstdint>

// Goodies builds (x4-pro-debug): cumulative activity counters for working out
// what each function costs in battery. The gauge only reports state of charge,
// so scripts/power_fit.py regresses the % drop between /debug/logs/power.csv
// rows (src/util/PowerLog) on the change in these counters.
//
// Totals survive deep sleep and clean restarts in RTC memory. Counts since this
// boot live in DRAM and are added to the restored base, so hooks that fire
// before begin() (display, storage, frontlight bring-up) are not lost. A boot
// that could not restore the exact last totals (power loss, crash, stats reset)
// starts a new generation: rows of different generations are never compared.
//
// Hooks are callable from any task; lightSleepExit runs in the idle task
// inside the PM switch lock. Everything else is main loop only.
namespace PowerCounters {

// Radio states, sampled once a second by PowerLog.
enum RadioState : uint8_t {
  RADIO_OFF,
  RADIO_UP,     // driver on, not associated (scanning, connecting, ESP-NOW)
  RADIO_PS,     // associated station, modem sleep
  RADIO_AWAKE,  // associated station, power save off
  RADIO_AP,     // soft AP (never sleeps)
  RADIO_STATES
};

// HalDisplay refresh kinds, for panel busy time.
enum PanelKind : uint8_t { PANEL_FULL, PANEL_HALF, PANEL_FAST, PANEL_GRAY, PANEL_KINDS };

struct Totals {
  uint64_t awakeMs;
  uint32_t asleepS[2];    // deep sleep: [0] charger wake off, [1] on (ext0 keeps RTC peripherals powered)
  uint64_t lightSleepUs;  // automatic light sleep while awake
  uint64_t maxClockUs;    // HalPowerManager's CPU max lock held
  uint64_t busyUs[2];     // per core, outside its idle task
  uint64_t wifiMs[RADIO_STATES];
  uint32_t wifiScans;
  uint32_t wifiConnects;
  uint64_t ipTxPackets;
  uint64_t ipRxPackets;
  uint64_t lightDutyMs;  // sum of duty (0-1023) x ms; / 1023 = ms at full light
  uint64_t panelBusyMs[PANEL_KINDS];
  uint64_t boosterMs;  // panel booster on (UC8179 powerOnIdle/powerOffIdle)
  uint64_t sdReadBytes;
  uint64_t sdWriteBytes;
  uint64_t sdUs;
};

#if CROSSDINK_GOODIES && !defined(SIMULATOR)
// Main loop, once per boot after the RTC clock is read. nowEpoch: UTC seconds
// (0 = unknown); deepSleepWake: this boot ended a deep sleep.
void begin(uint32_t nowEpoch, bool deepSleepWake);
// Main loop, about once a second: per-core busy time.
void tick();
// Main loop: the totals now (open segments included) and their generation.
Totals totals();
uint16_t generation();
// Main loop: totals to RTC memory. final: nothing more will count before
// deep sleep or the restart (sleep and restart paths only).
void save(bool final);
// Deep sleep is next. ext0: the charger STAT line will wake it.
void noteSleep(uint32_t nowEpoch, bool ext0);
// Stats reset: zero everything and start a new generation.
void reset();
// The Goodies > Knobs chargeWake value, kept in RTC memory for the wake paths
// that go back to sleep before the knobs are loaded. true when unknown.
void setChargeWakeAllowed(bool allowed);
bool chargeWakeAllowed();

// Hooks.
void maxClock(bool held);           // HalPowerManager::syncCpuFreqLock
void lightDuty(uint16_t duty1023);  // HalFrontlight::drive
void panelKind(PanelKind kind);     // HalDisplay::count, before the refresh
void panelBusy(bool begin);         // HalDisplay busy-wait hooks
void booster(bool on);              // HalDisplay refresh start, powerOnIdle/powerOffIdle, deepSleep
void sdRead(uint32_t bytes, uint32_t us);
void sdWrite(uint32_t bytes, uint32_t us);
void wifiMs(RadioState state, uint32_t ms);
void wifiScan();
void wifiConnect();
void ipPackets(uint32_t tx, uint32_t rx);
#else
inline void begin(uint32_t, bool) {}
inline void tick() {}
inline Totals totals() { return Totals{}; }
inline uint16_t generation() { return 0; }
inline void save(bool) {}
inline void noteSleep(uint32_t, bool) {}
inline void reset() {}
inline void setChargeWakeAllowed(bool) {}
inline bool chargeWakeAllowed() { return true; }
inline void maxClock(bool) {}
inline void lightDuty(uint16_t) {}
inline void panelKind(PanelKind) {}
inline void panelBusy(bool) {}
inline void booster(bool) {}
inline void sdRead(uint32_t, uint32_t) {}
inline void sdWrite(uint32_t, uint32_t) {}
inline void wifiMs(RadioState, uint32_t) {}
inline void wifiScan() {}
inline void wifiConnect() {}
inline void ipPackets(uint32_t, uint32_t) {}
#endif

}  // namespace PowerCounters
