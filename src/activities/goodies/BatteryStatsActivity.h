#pragma once

#include <HalStorage.h>

#include <cstdint>
#include <memory>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/BatteryLogParser.h"

// Goodies > Battery & stats: a battery % graph (bar under it = awake) and counters,
// all from the battery log (battery.3.csv down to battery.csv) so they match the web
// Battery page. A "stats_reset" row restarts the counters; the graph keeps going.
// Everything is read on the main loop in onEnter()/reset; render() only draws.
// battery.sum saves the parse after a load that read over 32 KB, so the next
// load reads only the rows after it, and its counters outlive the rotated-out files.
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
  using Point = BatteryLogParser::Point;
  static constexpr int MAX_LINES = 16;

  static constexpr size_t LOAD_BUF_BYTES = 4096;
  static constexpr uint32_t LOAD_STEP_MS = 20;  // per loop(), so input stays responsive

  void startLoad();
  bool resumeFromSum();  // battery.sum matches a log file: resume after its last row
  void step(uint32_t budgetMs);
  void buildLines();
  Rect resetRect() const;    // touch builds: "Reset" at the header's right end
  Rect refreshRect() const;  // and "Refresh" left of it
  void refresh();
  void confirmReset();

  BatteryLogParser parser{};
  HalFile file;
  std::unique_ptr<char[]> buf;
  size_t fill = 0;
  int fileIndex = 0;     // BatteryLog::LOG_PATHS index being read; counts down to 0 = battery.csv
  uint32_t fileOff = 0;  // bytes of that file read up to its last full row
  int sumFile = -1;      // where the last full row read ends: file index and offset
  uint32_t sumOff = 0;
  bool loading = false;
  uint32_t loadStartMs = 0;  // for the "read N B in M ms" log line
  uint32_t loadBytes = 0;
  char lines[MAX_LINES][80] = {};
  int lineCount = 0;
  int scroll = 0;           // first line drawn
  bool more = false;        // lines were cut off below
  uint16_t builtState = 0;  // estimateState() the estimate line was built for
  uint16_t seenState = 0;   // last estimateState() seen in loop(), and since when
  uint32_t seenMs = 0;
};
