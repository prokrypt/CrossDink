#pragma once
#include <FreeInkUIGfxRenderer.h>
#include <GfxRenderer.h>
#include <Knobs.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

enum class InputType { Text, Password, Url };

// Text entry on the FreeInkUI keyboard component: the SDK layout tables and
// keyboard() do the key rendering and hit-rect registration, InteractionBuffer
// routes taps/long-presses, and this activity owns the text field, cursor
// editing, and the URL snippet layouts.
class KeyboardEntryActivity : public Activity {
 public:
  explicit KeyboardEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::string title = "Enter Text", std::string initialText = "",
                                 const size_t maxLength = 0, InputType inputType = InputType::Text,
                                 const size_t minLength = 0)
      : Activity("KeyboardEntry", renderer, mappedInput),
        title(std::move(title)),
        text(std::move(initialText)),
        maxLength(maxLength),
        inputType(inputType),
        minLength(minLength) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool batchesInputDuringRefresh() const override { return true; }

 private:
  std::string title;
  std::string text;
  size_t maxLength;
  InputType inputType;
  size_t minLength;
  bool passwordVisible = false;

  // EXPERIMENT (test/kbd-uc8179): UC8179 keyboard refresh toggles.
  // Skip the ~27 ms OLD-plane re-stream after DU frames: the controller copies
  // NEW to OLD itself (CDI N2OCP, confirmed by the Goodies N2OCP probe).
  static constexpr uint8_t KBD_EXP_SKIP_RESYNC = 1;
  // 2 was T3 (windowed NEW upload); retired, every frame uploads the whole plane.
  static constexpr uint8_t KBD_EXP_DU_LUT = 4;
  static constexpr uint8_t KBD_EXP_HALF_ON_CLOSE = 8;
  static constexpr uint8_t KBD_EXP_HALF_ON_OPEN = 16;
  // No key highlight on touch taps: a keystroke changes only the text field.
  static constexpr uint8_t KBD_EXP_NO_TAP_HIGHLIGHT = 32;
  // Open with a plain OTP Fast first frame (no DU): the one-way DU drive adds
  // charge over the previous screen instead of clearing it, and 16 flashes.
  static constexpr uint8_t KBD_EXP_OTP_ON_OPEN = 64;
  // Trial: light-sleep through the refresh busy-wait (HalDisplay::setRefreshLightSleep).
  static constexpr uint8_t KBD_EXP_LIGHT_SLEEP_DRF = 128;
  // Settings > Turbo keyboard: full-frame DU typing (4; the SDK's DU LUT is
  // charge-balanced, two phases) with no OLD re-stream (1), no tap highlight
  // (32), OTP Fast first frame (64); the screen below redraws with OTP Fast on
  // exit. CMD:KBDEXP 100 = the same with the re-stream.
  static constexpr uint8_t KBD_EXP_TURBO_KEYBOARD =
      KBD_EXP_SKIP_RESYNC | KBD_EXP_DU_LUT | KBD_EXP_NO_TAP_HIGHLIGHT | KBD_EXP_OTP_ON_OPEN;
  uint8_t kbdExpFlags = 0;
  // DU frames per phase (two phases): KNOBS.kbdFrames, read per frame (default 6;
  // user 10/1 on 88859a0: acceptable gray and ghosting, 4 ghosts). Non-zero here
  // is a CMD:KBDEXP override.
  uint8_t kbdExpFrames = 0;
  uint8_t kbdExpPll = 0;
  bool kbdExpFirstFrame = true;
  std::atomic<unsigned long> strokeAtMs{0};
  // What queued the next keyboard frame, for the [KBD] line.
  enum class StrokeCause : uint8_t { Redraw, Key, Press, Release };
  std::atomic<uint8_t> strokeCause{0};
  uint32_t kbdFrame = 0;
  unsigned long prevFrameStrokeMs = 0;  // stroke behind the previous frame; 0 = none
  // Touch-down highlight is held back briefly: a quick tap releases first, and
  // its activation frame is then the keystroke's only refresh.
  static KNOB_ALIAS(TOUCH_HIGHLIGHT_DELAY_MS, kbdHighlightDelayMs);  // Goodies > Knobs
  bool highlightPending = false;
  unsigned long highlightDueMs = 0;
  void loadKbdExperiment();

 public:
  // Debug builds (serial CMD:KBDEXP): overrides the Turbo keyboard preset at
  // the next keyboard open, so the trial bits (64, 128) and a PLL choice stay
  // reachable without a file. Held in RAM until cleared or reboot.
  static void setExperimentOverride(uint8_t flags, uint8_t frames, uint8_t pll);
  static void clearExperimentOverride();

 private:
  void requestStrokeUpdate(StrokeCause cause = StrokeCause::Key);

  ButtonNavigator buttonNavigator;

  // Keyboard layers. The letter/symbol layers come from the SDK's builtin
  // layouts (with the always-visible number row); the URL layers are
  // app-defined tables in the .cpp.
  freeink::ui::KeyboardLayoutId layoutId = freeink::ui::KeyboardLayoutId::QwertyEn;
  bool showLangKey = false;
  bool shifted = false;
  bool symbols = false;
  bool urlPanel = false;  // URL snippet panel replaces the letter layer

  // Key hit rects registered by the keyboard component during render();
  // loop() routes touch snapshots against them. Cyrillic's wider rows register
  // 48 keys, so 56 retains headroom for the double-buffered interaction table.
  freeink::ui::InteractionBuffer<56> interactions;

  // GPIO selection over the current layout grid (row/col in layout terms;
  // the bottom action row is just the last row).
  int selRow = 0;
  int selCol = 0;
  bool selectionShown = true;  // false on touch until a button moves the selection
  // Shows a hidden selection; true when it did (the press only reveals it).
  bool revealSelection();
  // Button devices: Up/Down move the key row; long Up enters cursor mode.
  void handleUpDownButtons();

  bool confirmHeld = false;
  bool confirmLongHandled = false;

  bool cursorMode = false;
  bool togglePos = false;
  size_t cursorPos = 0;  // byte offset into text (always on a code point boundary)
  bool upHeld = false;
  bool upLongHandled = false;
  bool downHeld = false;
  bool downLongHandled = false;
  bool rightHeld = false;
  bool rightLongHandled = false;
  size_t savedCursorPos = 0;
  size_t rightStartCursorPos = 0;

  // Tap/hold routing (threshold long-press, release swallow, slide re-arm)
  // lives in the SDK; loop() feeds it the level-triggered touch state.
  freeink::ui::TouchHoldRouter touchRouter;

  // loop() runs on the main task while render() rebuilds the interaction
  // table on the render task. This is only the first-published-table gate;
  // later renders publish into the SDK's double buffer without clearing it,
  // so the previous complete table remains routable during a rebuild. Atomic
  // release/acquire ordering pairs the first publish with the main task.
  std::atomic<bool> interactionsReady{false};

  int delPressCount = 0;
  bool hintVisible = false;
  unsigned long hintShowTime = 0;

  enum class InputFieldTouchTarget { None, Cursor, PasswordToggle };

  void onComplete(std::string text);
  bool injectText(const char* utf8) override;
  void onCancel();
  InputFieldTouchTarget inputFieldTouchTargetFromPoint(int x, int y, size_t& position) const;
  std::string displayTextForCurrentState() const;
  // Advance of s[start, end) measured in place by temporarily null-terminating
  // at `end` — avoids a substr temporary per measurement.
  int measureRange(std::string& s, int start, int end) const;
  bool rangeIsRtl(std::string& s, int start, int end) const;
  // Largest line end in (start, s.length()] whose advance fits maxWidth.
  // Binary search over the monotonic prefix advance; always advances at least
  // one byte so an oversized glyph cannot stall the wrap loop.
  int lineBreakEnd(std::string& s, int start, int maxWidth) const;

  const freeink::ui::KeyboardLayout& currentLayout() const;
  const freeink::ui::KeyboardKey* selectedKey() const;
  int selectedLogicalIndex() const;
  void clampSelection();
  void moveSelectionRow(int delta);
  void moveSelectionCol(int delta);
  bool syncSelectionToValue(int16_t value);
  // Handles one key activation (by stable key id). Returns true when the
  // screen needs a repaint; OK/cancel finish the activity instead.
  bool activateValue(int16_t value, bool longPress);
  bool clearAllOrAltOnSelected();

  void insertUtf8(const char* out);
  bool backspaceUtf8();
  static size_t utf8Prev(const std::string& s, size_t pos);
  static size_t utf8Next(const std::string& s, size_t pos);

  freeink::ui::Rect keyboardRect() const;

  static KNOB_ALIAS(LONG_PRESS_MS, kbdHoldMs);  // Goodies > Knobs, as the three below
  static KNOB_ALIAS(DEL_LONG_PRESS_MS, kbdDelHoldMs);
  static KNOB_ALIAS(TOUCH_LONG_PRESS_MS, kbdTouchHoldMs);
  static KNOB_ALIAS(TOUCH_DEL_LONG_PRESS_MS, kbdTouchDelHoldMs);

  // App-specific key id: toggles the URL snippet panel (URL fields only).
  static constexpr int16_t URL_PANEL_KEY = -3;
};
