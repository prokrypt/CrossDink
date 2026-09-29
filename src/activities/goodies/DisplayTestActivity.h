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
  unsigned long resumeAtMs = 0;
  int pc = 0;
  std::vector<Loop> loops;
  std::vector<std::string> answers;

  // Refresh parameters set by the script.
  uint8_t duFrames = 6;
  uint8_t pll = 0;
  bool resync = true;
  bool windowOn = false;
  int window[4] = {};

  int refreshCount = 0;
  ModeStats stats[4];

  void runOps();
  void drawOp(const display_script::Op& op);
  void refresh(display_script::Mode mode);
  void drawFittedLine(const char* text, int y, bool bold);
  void drawAsk();
  void drawResult();
  void answer(int option);
};
