#pragma once

#include <cstdint>

#include "activities/Activity.h"
#include "util/WorkerTask.h"

// Goodies > Power test (X4 Pro debug builds): holds one load steady for
// Goodies > Knobs powerTestMin while everything else stays idle, with
// test_start / test_end rows in /debug/logs/power.csv, so scripts/power_fit.py
// gets intervals where one function is the only thing changing. Normal use
// rarely separates them: a transfer brings the radio, the full clock and SD
// writes together.
//
// The voltage sag probe instead steps each load on and off a few times and
// reads the cell voltage around each step: the drop is the load's current
// times the cell's internal resistance, a fast relative comparison.
//
// The device stays awake (auto sleep held off) until the run ends; Back stops
// it early and logs test_abort.
class PowerTestActivity final : public Activity {
 public:
  enum Test : uint8_t {
    IDLE,
    LIGHT_50,
    LIGHT_100,
    WIFI_PS,
    WIFI_AWAKE,
    CPU_BUSY,
    FAST_LOOP,
    FULL_LOOP,
    SAG_PROBE,
    TEST_COUNT
  };
  static const char* label(int test);  // Goodies list row
  static const char* tag(int test);    // power.csv detail

  PowerTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, int test)
      : Activity("PowerTest", renderer, mappedInput), test(static_cast<Test>(test < TEST_COUNT ? test : IDLE)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // True for one loop pass every few seconds while running: enough to hold
  // off the sleep timeout, without the fast loop tick a steady true brings.
  bool preventAutoSleep() override { return sleepBlockPulse; }
  // The radio is this screen's: the Wi-Fi remote and auto sync leave it alone.
  bool usesWifi() const override { return true; }
  // Modem sleep test: let the CPU idle and light-sleep with the link up, as an
  // idle File Transfer does.
  bool allowsRadioIdleSleep() override { return test == WIFI_PS && phase == Phase::Running; }
  bool skipLoopDelay() override { return test == SAG_PROBE && phase == Phase::Running; }

 private:
  enum class Phase : uint8_t { Joining, Running, Done, Failed };
  enum class Sag : uint8_t { Settle, Base, Apply, Load, Next };
  enum Load : uint8_t { LOAD_LIGHT, LOAD_CPU, LOAD_WIFI, LOAD_REFRESH, LOADS };
  static constexpr int SAG_CYCLES = 3;
  static constexpr int MAX_LINES = 16;

  void begin();  // the load is up: start the clock and log test_start
  void end(bool aborted);
  void startSpin();
  void stopSpin();
  bool joinWifi();
  void radioOff();
  void sagStep();
  void setLoad(Load load, bool on);
  void buildLines();
  uint16_t readMv();

  Test test;
  Phase phase = Phase::Joining;
  uint32_t phaseMs = 0;  // millis() the phase (or the run) began
  uint32_t runMs = 0;    // run length
  uint32_t endedMs = 0;
  uint32_t lastPulseMs = 0;
  bool sleepBlockPulse = false;
  bool wifiOn = false;
  bool pattern = false;  // refresh loops: which half is black
  uint32_t lastLoopMs = 0;
  bool fullNext = false;  // the next frame uses a Full refresh
  const char* failure = nullptr;

  // At begin(): battery and counters, for the result lines.
  uint16_t startPct256 = 0;
  uint16_t startMv = 0;
  struct Snapshot {
    uint64_t awakeMs, lsMs, maxMs, lightFullMs, wifiMs, refreshes;
  } start{};
  // At end(): the result lines stay as the run left them while the page is open.
  uint16_t endPct256 = 0;
  uint16_t endMv = 0;
  Snapshot endSnap{};
  Snapshot snapshot() const;

  // Sag probe: per load, the sum and sum of squares of the drop (mV) over cycles.
  Sag sag = Sag::Settle;
  uint8_t sagLoad = 0;
  uint8_t sagCycle = 0;
  uint32_t sagMs = 0;
  uint32_t sampleSum = 0;
  uint16_t sampleCount = 0;
  uint32_t lastSampleMs = 0;
  float baseMv = 0;
  float dropSum[LOADS] = {};
  float dropSq[LOADS] = {};
  uint8_t dropN[LOADS] = {};
  // Spread of single reads within a base window. Double: mV^2 near 1.6e7 leaves
  // a float about 1 mV^2 of resolution, the size of the noise it measures.
  double noiseSq = 0;
  uint16_t noiseN = 0;
  double windowSq = 0;

  WorkerTask spin;
  char lines[MAX_LINES][80] = {};
  int lineCount = 0;
};
