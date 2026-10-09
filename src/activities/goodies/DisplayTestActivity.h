#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "DisplayScript.h"
#include "activities/Activity.h"

// Runs one Display test line script (built-in or /debug/display/*.txt) and
// shows its per-mode refresh timings. Every refresh also logs a [GDY] line.
class DisplayTestActivity final : public Activity {
 public:
  // builtIn >= 0 selects display_script::BUILT_INS[builtIn]; otherwise `path`.
  DisplayTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title, int builtIn,
                      std::string path);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return phase.load() != Phase::Finished; }
  // The result screen is static: idle power-off drops the booster after a test.
  bool powerOffPanelWhenIdle() const override { return phase.load() == Phase::Finished; }

 private:
  enum class Phase : uint8_t { Running, Waiting, Asking, Finished };
  struct ModeStats {
    uint16_t count = 0;
    uint32_t upload = 0, drf = 0, sync = 0, total = 0;
  };
  struct Loop {
    int repeat;
    int remaining;
  };

  std::string title;
  int builtIn;
  std::string path;
  display_script::Script script;

  std::atomic<Phase> phase{Phase::Running};
  std::atomic<bool> stopRequested{false};
  bool stopped = false;
  bool askDrawn = false;
  int pickIndex = 0;  // button selection while a pick waits
  unsigned long resumeAtMs = 0;
  bool tapWait = false;    // Waiting for a tap or Confirm, not resumeAtMs
  int8_t savedNight = -1;  // the user's Night Mode while `night on` holds (-1: not held)
  int pc = 0;
  std::vector<Loop> loops;
  std::vector<std::string> answers;

  // Refresh parameters set by the script.
  uint8_t duFrames = 6;
  uint8_t pll = 0;

  int refreshCount = 0;
  ModeStats stats[4];

  struct BandLine {
    std::string text;
    bool bold;
  };
  // Flash ducks pages: the kind's dim/restore knobs with -/+ and an Again button.
  struct Hit {
    int x, y, w, h;
  };
  int duckKind = -1;       // display_script Ducks a0 of the current step (-1: none)
  int duckPc = 0;          // the step's Ducks op
  int rerunPc = 0;         // where Again replays from
  int skipTapsUntil = -1;  // an Again replay runs its earlier steps without waiting for taps
  int labelPc = 0;         // the page's Label op, redrawn when a value changes
  bool redrawLabel = false;
  Hit duckHit[6] = {};  // dim -, dim +, restore -, restore +, Again, Next
  int bandH = 0;        // height of the label/ask band in the framebuffer (0 after a fill)

  void runOps();
  void drawOp(const display_script::Op& op);
  void refresh(display_script::Mode mode);
  void grayPass();
  void setNight(bool on);
  void wrapBand(const std::string* parts, int count, std::vector<BandLine>& out) const;
  int drawBand(const std::vector<BandLine>& lines, int extraH);
  void drawDuckControls(int y);
  bool duckTap(int x, int y);
  void drawAsk();
  void drawResult();
  void answer(int option);
};
