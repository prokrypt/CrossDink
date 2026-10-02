#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"

// Goodies > Battery & stats: a battery % graph (bar under it = awake) and counters,
// all from the battery log (battery.1.csv, then battery.csv) so they match the web
// Battery page. A "stats_reset" row restarts the counters; the graph keeps going.
// Everything is read on the main loop in onEnter()/reset; render() only draws.
class BatteryStatsActivity final : public Activity {
 public:
  BatteryStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : Activity("BatteryStats", renderer, mappedInput) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool powerOffPanelWhenIdle() const override { return true; }
  // Reading the log runs a slice per loop pass: no idle wait between slices.
  bool skipLoopDelay() override { return loading; }

 private:
  struct Point {
    uint32_t epoch;
    uint8_t pct;
    bool awake;  // awake until the next row (not a sleep row or a charger row logged "asleep")
  };
  // Counted from the log since its first row or the last stats_reset row.
  struct LogStats {
    uint32_t first, last;  // epochs
    bool reset;            // first is a stats_reset row
    uint32_t coldBoots, restarts, wakes, falseWakes;
    uint32_t awakeS, asleepS;  // power-off gaps before a cold boot count as neither
    // Last charge session: rows with USB or charging, merged across gaps under
    // CHARGE_MERGE_S (USB flapping). chargedEpoch is its end, 0 = none yet.
    uint32_t chargedEpoch, chargeStartEpoch;
    uint16_t chargeFromC, chargeToC;  // 0.01 %
    bool chargeFromFine, chargeToFine, charging;
    // On battery, over the whole log; [0] awake, [1] asleep. In 0.01 %: drop,
    // the part of it from whole-percent rows, and its ± (each unbroken run of
    // steps adds its rows' precision, 1 or 100: inside a run the roundings cancel).
    uint32_t battS[2], dropC[2], coarseC[2], errC[2];
    int8_t run;  // category of the run the last step extended, -1 = none
    bool runFine;
    // The open stretch of on-battery steps: net drop per category, signed, so
    // the gauge's rise after an unplug cancels drops instead of being ignored.
    // Added to dropC (a negative net as 0) when the stretch ends.
    int32_t netC[2], netCoarseC[2];
    // Awake on battery, between fractional rows, by state [Wi-Fi * 2 + light on]:
    // signed drop in 0.01 %, seconds, and light % x seconds. The first
    // UNPLUG_SKIP_S after a charge is left out (the gauge rises then).
    int32_t stateDropC[4];
    uint32_t stateS[4];
    uint64_t stateLight[4];
  };
  static void endStretch(LogStats& s);
  static constexpr uint32_t CHARGE_MERGE_S = 60;
  static constexpr uint32_t UNPLUG_SKIP_S = 1800;
  static constexpr int MAX_POINTS = 400;
  static constexpr int MAX_LINES = 16;

  static constexpr size_t LOAD_BUF_BYTES = 4096;
  static constexpr uint32_t LOAD_STEP_MS = 20;    // per loop(), so input stays responsive

  void startLoad();
  void step(uint32_t budgetMs);
  void parseRow(const char* line);
  void buildLines();
  Rect resetRect() const;    // touch builds: "Reset" at the header's right end
  Rect refreshRect() const;  // and "Refresh" left of it
  void refresh();
  void confirmReset();

  Point points[MAX_POINTS];
  LogStats st{};
  Point prev{};  // last row read, carried across the two files
  uint16_t prevC = 0;     // drop reference % in 0.01 % (the previous row, or the last fractional one)
  bool prevFine = false;  // that % had a fraction
  bool prevUsb = false;
  uint16_t prevRowC = 0;  // the previous row's % in 0.01 %, its precision, Wi-Fi and light
  bool prevRowFine = false;
  bool prevWifi = false;
  uint8_t prevLight = 0;
  HalFile file;
  std::unique_ptr<char[]> buf;
  size_t fill = 0;
  int fileIndex = 0;  // 0 = battery.1.csv, 1 = battery.csv
  bool loading = false;
  uint32_t loadStartMs = 0;  // for the "read N B in M ms" log line
  uint32_t loadBytes = 0;
  int pointCount = 0;
  char lines[MAX_LINES][80];
  int lineCount = 0;
  int scroll = 0;     // first line drawn
  bool more = false;  // lines were cut off below
  uint16_t builtState = 0;  // estimateState() the estimate line was built for
  uint16_t seenState = 0;   // last estimateState() seen in loop(), and since when
  uint32_t seenMs = 0;
};
