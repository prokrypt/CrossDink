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
    uint32_t chargedEpoch;     // last "charged" row, 0 = none
    uint8_t chargedPct;
    // On battery since charging last stopped ("charged" or "chg_off").
    uint32_t battAwakeS, battAsleepS, dropAwakePct, dropAsleepPct;
  };
  static constexpr int MAX_POINTS = 400;
  static constexpr int MAX_LINES = 16;

  static constexpr size_t LOAD_BUF_BYTES = 4096;
  static constexpr uint32_t LOAD_FIRST_MS = 150;  // onEnter, before the first draw
  static constexpr uint32_t LOAD_STEP_MS = 20;    // per loop(), so input stays responsive

  void startLoad();
  void step(uint32_t budgetMs);
  void parseRow(const char* line);
  void buildLines();
  Rect resetRect() const;  // touch builds: "Reset" at the header's right end
  void confirmReset();

  Point points[MAX_POINTS];
  LogStats st{};
  Point prev{};  // last row read, carried across the two files
  bool prevUsb = false;
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
};
