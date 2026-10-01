#pragma once

#include <cstdint>

#include "activities/Activity.h"

// Goodies > Battery & stats: a battery % graph from the tail of
// /debug/logs/battery.csv (bar under it = asleep) and a page of counters. Everything
// is read on the main loop in onEnter()/reset; render() only draws.
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
  static constexpr int MAX_POINTS = 400;
  static constexpr int MAX_LINES = 12;

  void loadGraph();
  void buildLines();

  Point points[MAX_POINTS];
  int pointCount = 0;
  char lines[MAX_LINES][80];
  int lineCount = 0;
};
