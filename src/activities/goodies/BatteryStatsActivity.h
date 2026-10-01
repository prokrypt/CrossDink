#pragma once

#include <cstdint>

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
  void loop() override;
  void render(RenderLock&&) override;
  bool powerOffPanelWhenIdle() const override { return true; }

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

  void loadLog();
  void readLog(const char* path);
  void buildLines();
  Rect resetRect() const;  // touch builds: "Reset" at the header's right end
  void confirmReset();

  Point points[MAX_POINTS];
  LogStats st{};
  Point prev{};  // last row read, carried across the two files
  bool prevUsb = false;
  int pointCount = 0;
  char lines[MAX_LINES][80];
  int lineCount = 0;
  int scroll = 0;     // first line drawn
  bool more = false;  // lines were cut off below
};
