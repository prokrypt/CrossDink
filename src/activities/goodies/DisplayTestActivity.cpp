#include "DisplayTestActivity.h"

#if CROSSDINK_GOODIES

#include <FreeInkDisplay.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

using display_script::Mode;
using display_script::Op;
using display_script::OpCode;

namespace {
constexpr size_t MAX_SCRIPT_BYTES = 8 * 1024;
constexpr char SAMPLE_TEXT[] = "The quick brown fox jumps over the lazy dog 0123456789";
}  // namespace

DisplayTestActivity::DisplayTestActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string title,
                                         const int builtIn, std::string path)
    : Activity("DisplayTest", renderer, mappedInput),
      title(std::move(title)),
      builtIn(builtIn),
      path(std::move(path)) {}

void DisplayTestActivity::onEnter() {
  Activity::onEnter();
  if (builtIn >= 0 && builtIn < display_script::BUILT_IN_COUNT) {
    const char* src = display_script::BUILT_INS[builtIn].source;
    script = display_script::parse(src, strlen(src));
  } else {
    // 8 KB covers any hand-written test; freed as soon as it is parsed.
    auto buf = makeUniqueNoThrow<char[]>(MAX_SCRIPT_BYTES);
    const size_t len = buf ? Storage.readFileToBuffer(path.c_str(), buf.get(), MAX_SCRIPT_BYTES) : 0;
    if (len == 0) {
      LOG_ERR("GDY", "cannot read %s", path.c_str());
      script.errorLine = 1;
      script.error = "cannot read file";
    } else {
      script = display_script::parse(buf.get(), len);
    }
  }
  if (!script.name.empty()) title = script.name;
  if (script.error) {
    LOG_ERR("GDY", "test=\"%s\" parse error line %d: %s", title.c_str(), script.errorLine, script.error);
    phase = Phase::Finished;
  } else {
    LOG_INF("GDY", "test=\"%s\" start ops=%u", title.c_str(), static_cast<unsigned>(script.ops.size()));
    phase = Phase::Running;
  }
  loops.reserve(4);
  requestUpdate();
}

void DisplayTestActivity::onExit() {
#ifndef SIMULATOR
  freeink::setUc8179KbdExperiment(nullptr);
#endif
  Activity::onExit();
}

void DisplayTestActivity::loop() {
  const Phase current = phase.load();
  const bool back = mappedInput.wasReleased(MappedInputManager::Button::Back) ||
                    (current == Phase::Finished && TouchHeaderBackButton::wasTapped(mappedInput, renderer));
  if (back) {
    if (current == Phase::Finished) {
      finish();
      return;
    }
    // A running batch checks this between ops; a waiting or asking test
    // stops on its next render.
    stopRequested = true;
    if (current != Phase::Running) {
      phase = Phase::Running;
      requestUpdate();
    }
    return;
  }
  if (current == Phase::Waiting && static_cast<long>(millis() - resumeAtMs) >= 0) {
    phase = Phase::Running;
    requestUpdate();
    return;
  }
  if (current == Phase::Asking && askDrawn) {
    int x = 0, y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      answer(0);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      answer(1);
    } else if (mappedInput.wasScreenTapped(x, y)) {
      answer(x < renderer.getScreenWidth() / 2 ? 0 : 1);
    }
  }
}

void DisplayTestActivity::answer(const int option) {
  const Op& op = script.ops[pc];
  LOG_INF("GDY", "test=\"%s\" ask=\"%s\" answer=\"%s\"", title.c_str(), op.text.c_str(), op.options[option].c_str());
  answers.push_back(op.text + " " + op.options[option]);
  ++pc;
  askDrawn = false;
  phase = Phase::Running;
  requestUpdate();
}

void DisplayTestActivity::render(RenderLock&&) {
  if (phase.load() == Phase::Running) runOps();
  const Phase current = phase.load();
  if (current == Phase::Asking && !askDrawn) {
    drawAsk();
  } else if (current == Phase::Finished) {
    drawResult();
  }
}

void DisplayTestActivity::runOps() {
  while (pc < static_cast<int>(script.ops.size())) {
    if (stopRequested) {
      stopped = true;
      LOG_INF("GDY", "test=\"%s\" stopped at op %d", title.c_str(), pc);
      break;
    }
    const Op& op = script.ops[pc];
    switch (op.code) {
      case OpCode::Refresh:
        refresh(static_cast<Mode>(op.a[0]));
        break;
      case OpCode::Frames:
        duFrames = static_cast<uint8_t>(op.a[0]);
        break;
      case OpCode::Pll:
        pll = static_cast<uint8_t>(op.a[0]);
        break;
      case OpCode::Resync:
        resync = op.a[0] != 0;
        break;
      case OpCode::Window:
        windowOn = op.a[2] > 0;
        for (int i = 0; i < 4; ++i) window[i] = op.a[i];
        break;
      case OpCode::Scrub:
#ifndef SIMULATOR
        // Applies to the next Fast/DU refresh; a DU scrub needs `refresh du`.
        if (op.a[0]) {
          freeink::requestUc8179DuScrubNext();
        } else {
          freeink::requestUc8179HalfNext();
        }
#endif
        break;
      case OpCode::Wait:
        resumeAtMs = millis() + op.a[0];
        ++pc;
        phase = Phase::Waiting;
        return;
      case OpCode::Repeat:
        loops.push_back({pc, op.a[0]});
        break;
      case OpCode::End: {
        Loop& loop = loops.back();
        if (--loop.remaining > 0) {
          pc = loop.repeat;  // ++pc below lands on the first body op
        } else {
          loops.pop_back();
        }
        break;
      }
      case OpCode::Note:
        LOG_INF("GDY", "test=\"%s\" note %s", title.c_str(), op.text.c_str());
        break;
      case OpCode::Ask:
        phase = Phase::Asking;
        return;
      case OpCode::Name:
        break;
      default:
        drawOp(op);
        break;
    }
    ++pc;
  }
  if (!stopped) LOG_INF("GDY", "test=\"%s\" done refreshes=%d", title.c_str(), refreshCount);
  phase = Phase::Finished;
}

void DisplayTestActivity::drawOp(const Op& op) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  switch (op.code) {
    case OpCode::Fill:
      renderer.clearScreen(op.a[0] ? 0x00 : 0xFF);
      break;
    case OpCode::Checker: {
      const int n = op.a[0];
      for (int y = 0; y < h; y += n) {
        for (int x = ((y / n) & 1) * n; x < w; x += 2 * n) renderer.fillRect(x, y, n, n);
      }
      break;
    }
    case OpCode::HStripes:
      for (int y = 0; y < h; y += 2 * op.a[0]) renderer.fillRect(0, y, w, op.a[0]);
      break;
    case OpCode::VStripes:
      for (int x = 0; x < w; x += 2 * op.a[0]) renderer.fillRect(x, 0, op.a[0], h);
      break;
    case OpCode::Box:
      renderer.fillRect(op.a[0], op.a[1], op.a[2], op.a[3], op.a[4] == 0);
      break;
    case OpCode::Text: {
      const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      for (int y = 4; y + lineHeight <= h; y += lineHeight) renderer.drawText(UI_12_FONT_ID, 8, y, SAMPLE_TEXT);
      break;
    }
    case OpCode::Invert:
      renderer.invertScreen();
      break;
    case OpCode::Label: {
      const int bandH = renderer.getLineHeight(UI_12_FONT_ID) + 12;
      renderer.fillRect(0, 0, w, bandH, false);
      renderer.fillRect(0, bandH - 2, w, 2);
      renderer.drawCenteredText(UI_12_FONT_ID, 6, op.text.c_str(), true, EpdFontFamily::BOLD);
      break;
    }
    default:
      break;
  }
}

void DisplayTestActivity::refresh(const Mode mode) {
  HalDisplay::RefreshMode halMode = HalDisplay::FAST_REFRESH;
  if (mode == Mode::Full) halMode = HalDisplay::FULL_REFRESH;
  if (mode == Mode::Half) halMode = HalDisplay::HALF_REFRESH;
  unsigned windows = 0;
#ifndef SIMULATOR
  // Window, resync and DU only apply to Fast-mode refreshes (the kbd-exp hooks).
  freeink::Uc8179KbdExperiment exp;
  if (!resync) exp.flags |= freeink::Uc8179KbdExperiment::SkipOldResync;
  if (mode == Mode::Du) {
    exp.flags |= freeink::Uc8179KbdExperiment::KbdLut;
    exp.lutFrames = duFrames;
    exp.pll = pll;
  }
  if (windowOn) {
    auto& win = exp.windows[0];
    if (renderer.toFrameBufferRect(window[0], window[1], window[2], window[3], win.x, win.y, win.w, win.h)) {
      exp.flags |= freeink::Uc8179KbdExperiment::TwoWindow;
      exp.windowCount = 1;
      windows = 1;
    }
  }
  freeink::setUc8179KbdExperiment(exp.flags ? &exp : nullptr);
  const uint32_t countBefore = freeink::uc8179KbdTiming().count;
#endif
  const unsigned long startMs = millis();
  renderer.displayBuffer(halMode);
  renderer.waitRefreshComplete();
  const auto totalMs = static_cast<uint32_t>(millis() - startMs);
  ++refreshCount;
  ModeStats& s = stats[static_cast<int>(mode)];
  s.count++;
  s.total += totalMs;
#ifndef SIMULATOR
  freeink::setUc8179KbdExperiment(nullptr);
  const freeink::Uc8179KbdTiming t = freeink::uc8179KbdTiming();
  const bool measured = t.count != countBefore;
  if (measured) {
    s.upload += t.uploadMs;
    s.drf += t.drfMs;
    s.sync += t.syncMs;
  }
  LOG_INF("GDY",
          "test=\"%s\" n=%d mode=%s upload=%ld drf=%ld sync=%ld rows=%u total=%u frames=%u pll=0x%02x win=%u "
          "resync=%d",
          title.c_str(), refreshCount, display_script::modeName(mode), measured ? static_cast<long>(t.uploadMs) : -1L,
          measured ? static_cast<long>(t.drfMs) : -1L, measured ? static_cast<long>(t.syncMs) : -1L,
          static_cast<unsigned>(t.drfRows), static_cast<unsigned>(totalMs), duFrames, pll, windows, resync ? 1 : 0);
#else
  (void)windows;
  LOG_INF("GDY", "test=\"%s\" n=%d mode=%s total=%u", title.c_str(), refreshCount, display_script::modeName(mode),
          static_cast<unsigned>(totalMs));
#endif
}

void DisplayTestActivity::drawAsk() {
  // A band over the test image; the left/right halves are the two answers.
  const Op& op = script.ops[pc];
  const int w = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int bandH = lineHeight * 3 + 16;
  renderer.fillRect(0, 0, w, bandH, false);
  renderer.drawRect(0, 0, w, bandH);
  renderer.drawCenteredText(UI_12_FONT_ID, 6, op.text.c_str(), true, EpdFontFamily::BOLD);
  char left[48];
  char right[48];
  snprintf(left, sizeof(left), "< %s", op.options[0].c_str());
  snprintf(right, sizeof(right), "%s >", op.options[1].c_str());
  const int y = 10 + lineHeight * 2;
  renderer.drawText(UI_12_FONT_ID, 16, y, left);
  renderer.drawText(UI_12_FONT_ID, w - 16 - renderer.getTextWidth(UI_12_FONT_ID, right), y, right);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  renderer.waitRefreshComplete();
  askDrawn = true;
}

void DisplayTestActivity::drawResult() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, header, title.c_str(), false);
  } else {
    GUI.drawHeader(renderer, header, title.c_str());
  }
  const int x = metrics.contentSidePadding;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 6;
  int y = header.y + header.height + metrics.verticalSpacing;
  char line[128];

  if (script.error) {
    snprintf(line, sizeof(line), "%s %d: %s", tr(STR_ERROR_MSG), script.errorLine, script.error);
  } else {
    snprintf(line, sizeof(line), "%s  %s: %d", stopped ? tr(STR_TEST_STOPPED) : tr(STR_DONE), tr(STR_REFRESHES),
             refreshCount);
  }
  renderer.drawText(UI_10_FONT_ID, x, y, line, true, EpdFontFamily::BOLD);
  y += lineHeight * 3 / 2;

  // Average ms per mode: upload / drf / sync / total.
  for (int m = 0; m < 4; ++m) {
    const ModeStats& s = stats[m];
    if (s.count == 0) continue;
    snprintf(line, sizeof(line), "%-4s x%-3u %4lu / %4lu / %4lu / %4lu ms",
             display_script::modeName(static_cast<Mode>(m)), static_cast<unsigned>(s.count),
             static_cast<unsigned long>(s.upload / s.count), static_cast<unsigned long>(s.drf / s.count),
             static_cast<unsigned long>(s.sync / s.count), static_cast<unsigned long>(s.total / s.count));
    renderer.drawText(UI_10_FONT_ID, x, y, line);
    y += lineHeight;
  }
  if (refreshCount > 0) {
    renderer.drawText(UI_10_FONT_ID, x, y, "upload / drf / sync / total (avg)", true, EpdFontFamily::ITALIC);
    y += lineHeight * 3 / 2;
  }
  for (const auto& a : answers) {
    renderer.drawText(UI_10_FONT_ID, x, y, a.c_str());
    y += lineHeight;
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Half clears whatever the test left on the panel.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

#endif  // CROSSDINK_GOODIES
