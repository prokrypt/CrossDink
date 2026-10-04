#include "DisplayTestActivity.h"

#if CROSSDINK_GOODIES

#include <FreeInkDisplay.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Knobs.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

using display_script::Mode;
using display_script::Op;
using display_script::OpCode;

namespace {
constexpr size_t MAX_SCRIPT_BYTES = 8 * 1024;
constexpr char SAMPLE_TEXT[] = "The quick brown fox jumps over the lazy dog 0123456789";
// Flash kinds by Ducks a0, with their knob ids (Knobs.def).
struct DuckKind {
  const char* name;
  const char* dimId;
  const char* restoreId;
};
constexpr DuckKind DUCK_KINDS[] = {{"Full", "flashFullDimMs", "flashFullRestoreMs"},
                                   {"Gray", "flashGrayDimMs", "flashGrayRestoreMs"},
                                   {"Paint", "flashPaintDimMs", "flashPaintRestoreMs"},
                                   {"GrayDark", "flashGrayDarkDimMs", "flashGrayDarkRestoreMs"}};
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
  setNight(false);
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
  int tapX = 0, tapY = 0;
  if (current == Phase::Waiting) {
    bool go;
    if (tapWait) {
      const bool tapped = mappedInput.wasScreenTapped(tapX, tapY);
      // Flash ducks pages: only the Next button (or OK) advances; other taps do nothing.
      go = (tapped && (duckKind < 0 || duckTap(tapX, tapY))) ||
           mappedInput.wasReleased(MappedInputManager::Button::Confirm);
    } else {
      go = static_cast<long>(millis() - resumeAtMs) >= 0;
    }
    if (go) {
      tapWait = false;
      phase = Phase::Running;
      requestUpdate();
      return;
    }
  }
  if (current == Phase::Asking && askDrawn && script.ops[pc].code == OpCode::Pick) {
    const Op& op = script.ops[pc];
    const int cells = static_cast<int>(op.options.size());
    int x = 0, y = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      answer(pickIndex);
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
               mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      const int step = mappedInput.wasReleased(MappedInputManager::Button::Left) ? cells - 1 : 1;
      pickIndex = (pickIndex + step) % cells;
      askDrawn = false;  // redraw the band with the new selection
      requestUpdate();
    } else if (mappedInput.wasScreenTapped(x, y) && x >= op.a[0] && y >= op.a[1]) {
      const int col = (x - op.a[0]) / op.a[2];
      const int row = (y - op.a[1]) / op.a[3];
      if (col < op.a[4] && row < op.a[5]) answer(row * op.a[4] + col);
    }
  } else if (current == Phase::Asking && askDrawn) {
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
  if (op.code == OpCode::Confirm && option == 0) stopRequested = true;
  if (op.code == OpCode::Pick) {
    LOG_INF("GDY", "test=\"%s\" pick=\"%s\" square=%d variant=\"%s\"", title.c_str(), op.text.c_str(), option + 1,
            op.options[option].c_str());
    answers.push_back(op.text + " -> " + std::to_string(option + 1) + " (" + op.options[option] + ")");
  } else {
    LOG_INF("GDY", "test=\"%s\" ask=\"%s\" answer=\"%s\"", title.c_str(), op.text.c_str(), op.options[option].c_str());
    answers.push_back(op.text + " -> " + op.options[option]);
  }
  pickIndex = 0;
  ++pc;
  askDrawn = false;
  phase = Phase::Running;
  requestUpdate();
}

void DisplayTestActivity::render(RenderLock&&) {
  if (phase.load() == Phase::Running) runOps();
  const Phase current = phase.load();
  if (redrawLabel && current == Phase::Waiting) {
    // A -/+ changed a value: the band again, with the new numbers.
    redrawLabel = false;
    drawOp(script.ops[labelPc]);
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    renderer.waitRefreshComplete();
  }
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
        pll = knobs::PLL_BYTES[op.a[0]];  // index checked when the script was parsed
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
      case OpCode::Tap:
        if (pc < skipTapsUntil) break;
        skipTapsUntil = -1;
        tapWait = true;
        ++pc;
        phase = Phase::Waiting;
        return;
      case OpCode::Rerun:
        rerunPc = pc;
        break;
      case OpCode::Ducks:
        if (pc < skipTapsUntil) break;  // an Again replay keeps the page's own kind
        duckKind = static_cast<int>(op.a[0]);
        duckPc = pc;
        break;
      case OpCode::Gray:
        grayPass();
        break;
      case OpCode::Night:
        setNight(op.a[0] != 0);
        break;
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
      case OpCode::Confirm:
      case OpCode::Pick:
        phase = Phase::Asking;
        return;
      case OpCode::Swing:
      case OpCode::Null:
#ifndef SIMULATOR
        if (op.code == OpCode::Swing) {
          freeink::requestUc8179HalfAsDuScrubNext(static_cast<uint8_t>(op.a[0]));
        } else {
          freeink::requestUc8179NullNext(static_cast<uint8_t>(op.a[0]));
        }
#endif
        refresh(op.code == OpCode::Swing ? Mode::Half : Mode::Fast);
        break;
      case OpCode::Name:
        break;
      default:
        if (op.code == OpCode::Label) LOG_INF("GDY", "test=\"%s\" label %s", title.c_str(), op.text.c_str());
        drawOp(op);
        break;
    }
    ++pc;
  }
  if (!stopped) LOG_INF("GDY", "test=\"%s\" done refreshes=%d", title.c_str(), refreshCount);
  setNight(false);
  phase = Phase::Finished;
}

void DisplayTestActivity::drawOp(const Op& op) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  switch (op.code) {
    case OpCode::Fill:
      renderer.clearScreen(op.a[0] ? 0x00 : 0xFF);
      bandH = 0;
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
      // Wrapped to the width so no line runs off the edge in either orientation.
      const auto lines = renderer.wrappedText(UI_12_FONT_ID, SAMPLE_TEXT, w - 16, 4);
      const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
      size_t i = 0;
      for (int y = 4; !lines.empty() && y + lineHeight <= h; y += lineHeight) {
        renderer.drawText(UI_12_FONT_ID, 8, y, lines[i++ % lines.size()].c_str());
      }
      break;
    }
    case OpCode::Invert:
      renderer.invertScreen();
      break;
    case OpCode::DrawText:
      renderer.drawText(UI_12_FONT_ID, op.a[0], op.a[1], op.text.c_str(), true, EpdFontFamily::BOLD);
      break;
    case OpCode::Label: {
      // An Again replay of an earlier step shows this page's label, not that step's.
      const Op& shown = duckKind >= 0 && pc < skipTapsUntil ? script.ops[labelPc] : op;
      if (&shown == &op) labelPc = static_cast<int>(&op - script.ops.data());
      // Up to 3 lines split on '|': what this is (bold), what to look for, what is next.
      std::string lines[3];
      int count = 0;
      for (size_t start = 0; count < 3 && start <= shown.text.size(); ++count) {
        const size_t bar = shown.text.find('|', start);
        const size_t stop = bar == std::string::npos ? shown.text.size() : bar;
        lines[count] = shown.text.substr(start, stop - start);
        lines[count].erase(0, lines[count].find_first_not_of(' '));
        lines[count].erase(lines[count].find_last_not_of(' ') + 1);
        start = stop + 1;
      }
      std::vector<BandLine> wrapped;
      wrapBand(lines, count, wrapped);
      if (duckKind >= 0) {
        const int rowH = renderer.getLineHeight(UI_12_FONT_ID) + 12;
        drawDuckControls(drawBand(wrapped, 3 * rowH) + 4);
      } else {
        drawBand(wrapped, 0);
      }
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
#ifndef SIMULATOR
  // DU only applies to Fast-mode refreshes (the kbd-exp hook): the gated,
  // balanced KW/WK register LUT; every other mode runs the panel OTP waveform.
  freeink::Uc8179KbdExperiment exp;
  if (mode == Mode::Du) {
    exp.flags |= freeink::Uc8179KbdExperiment::KbdLut;
    exp.lutFrames = duFrames;
    exp.pll = pll;
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
  LOG_INF("GDY", "test=\"%s\" n=%d mode=%s upload=%ld drf=%ld sync=%ld rows=%u total=%u frames=%u pll=0x%02x",
          title.c_str(), refreshCount, display_script::modeName(mode), measured ? static_cast<long>(t.uploadMs) : -1L,
          measured ? static_cast<long>(t.drfMs) : -1L, measured ? static_cast<long>(t.syncMs) : -1L,
          static_cast<unsigned>(t.drfRows), static_cast<unsigned>(totalMs), duFrames, pll);
#else
  LOG_INF("GDY", "test=\"%s\" n=%d mode=%s total=%u", title.c_str(), refreshCount, display_script::modeName(mode),
          static_cast<unsigned>(totalMs));
#endif
}

// The reader's overlay AA pass (ReaderUtils::renderAntiAliased) over the B/W
// page on the panel: dark gray on the left half, light gray on the right, under
// the label band. Masks are only meaningful over black pixels.
void DisplayTestActivity::grayPass() {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight() - bandH;
  if (!renderer.storeBwBuffer()) {
    LOG_ERR("GDY", "test=\"%s\" gray: no memory for the B/W copy", title.c_str());
    return;
  }
  const unsigned long startMs = millis();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
  renderer.fillRect(0, bandH, w / 2, h, false);  // dark: both masks
  renderer.copyGrayscaleLsbBuffers();
  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
  renderer.fillRect(0, bandH, w, h, false);  // light: MSB only
  renderer.copyGrayscaleMsbBuffers();
  renderer.displayGrayBuffer();
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.restoreBwBuffer();
  ++refreshCount;
  LOG_INF("GDY", "test=\"%s\" n=%d mode=gray total=%u", title.c_str(), refreshCount,
          static_cast<unsigned>(millis() - startMs));
}

// ActivityManager re-applies SETTINGS.screenInverted before every render, so
// Night Mode for the test goes through the setting (in RAM, never saved) and
// the user's value comes back when the test ends or exits.
void DisplayTestActivity::setNight(const bool on) {
  if (on == (savedNight >= 0)) return;
  if (on) {
    savedNight = static_cast<int8_t>(SETTINGS.screenInverted);
    SETTINGS.screenInverted = 1;
  } else {
    SETTINGS.screenInverted = static_cast<uint8_t>(savedNight);
    savedNight = -1;
  }
  renderer.setInvertedTextGray(on);  // overlay gray in panel polarity (as TxtReader's night mode)
  display.setInverted(SETTINGS.screenInverted != 0);
}

// Word-wraps band parts (the first bold) to the screen width in UI_12, up to 3
// lines per part.
void DisplayTestActivity::wrapBand(const std::string* parts, const int count, std::vector<BandLine>& out) const {
  const int maxWidth = renderer.getScreenWidth() - 16;
  for (int i = 0; i < count; ++i) {
    auto wrapped = renderer.wrappedText(UI_12_FONT_ID, parts[i].c_str(), maxWidth, 3,
                                        i == 0 ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    for (auto& line : wrapped) out.push_back({std::move(line), i == 0});
  }
}

// White band across the top: `lines` centered, then `extraH` px free below
// them. It also covers the previous band. Returns the y under the last line.
int DisplayTestActivity::drawBand(const std::vector<BandLine>& lines, const int extraH) {
  const int w = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int textBottom = 6 + static_cast<int>(lines.size()) * lineHeight;
  bandH = std::max(bandH, textBottom + extraH + 6);
  renderer.fillRect(0, 0, w, bandH, false);
  renderer.fillRect(0, bandH - 2, w, 2);
  int y = 6;
  for (const auto& line : lines) {
    renderer.drawCenteredText(UI_12_FONT_ID, y, line.text.c_str(), true,
                              line.bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    y += lineHeight;
  }
  return textBottom;
}

// Under the label: the kind's dim and restore offsets with -/+, then Again.
// Hit boxes are kept in logical coordinates for duckTap.
void DisplayTestActivity::drawDuckControls(int y) {
  const int w = renderer.getScreenWidth();
  const int lineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int rowH = lineHeight + 12;
  const int sq = rowH - 4;
  const DuckKind& k = DUCK_KINDS[duckKind];
  const char* const ids[2] = {k.dimId, k.restoreId};
  const char* const names[2] = {"dim", "restore"};
  auto button = [&](const Hit& r, const char* text) {
    renderer.drawRect(r.x, r.y, r.w, r.h);
    renderer.drawText(UI_12_FONT_ID, r.x + (r.w - renderer.getTextWidth(UI_12_FONT_ID, text)) / 2,
                      r.y + (r.h - lineHeight) / 2, text);
  };
  for (int i = 0; i < 2; ++i) {
    const int idx = knobs::find(ids[i]);
    char line[48];
    snprintf(line, sizeof(line), "%s %s %ld ms", k.name, names[i], idx >= 0 ? static_cast<long>(knobs::get(idx)) : 0L);
    renderer.drawText(UI_12_FONT_ID, 16, y + (sq - lineHeight) / 2, line);
    duckHit[i * 2] = {w - 16 - 2 * sq - 8, y, sq, sq};
    duckHit[i * 2 + 1] = {w - 16 - sq, y, sq, sq};
    button(duckHit[i * 2], "-");
    button(duckHit[i * 2 + 1], "+");
    y += rowH;
  }
  const int half = (w - 32 - 8) / 2;
  duckHit[4] = {16, y, half, sq};
  duckHit[5] = {16 + half + 8, y, half, sq};
  button(duckHit[4], "Again");
  button(duckHit[5], "Next");
}

// A tap on a control: -/+ change the knob (RAM now, knobs.json when the screen
// closes) and redraw the numbers; Again replays the step from its start; Next
// returns true (advance). Any other tap does nothing.
bool DisplayTestActivity::duckTap(const int x, const int y) {
  for (int i = 0; i < 6; ++i) {
    const Hit& r = duckHit[i];
    if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) continue;
    if (i == 5) return true;  // Next
    if (i == 4) {
      skipTapsUntil = duckPc;  // earlier steps of a replay run on without a tap
      pc = rerunPc;
      tapWait = false;
      phase = Phase::Running;
    } else {
      const DuckKind& k = DUCK_KINDS[duckKind];
      const int idx = knobs::find(i < 2 ? k.dimId : k.restoreId);
      if (idx >= 0) knobs::set(idx, knobs::get(idx) + (i & 1 ? 1 : -1) * knobs::INFO[idx].step);
      redrawLabel = true;
    }
    requestUpdate();
    return false;
  }
  return false;
}

void DisplayTestActivity::drawAsk() {
  // A band over the test image; the left/right halves are the two answers.
  const Op& op = script.ops[pc];
  const int w = renderer.getScreenWidth();
  std::vector<BandLine> lines;
  wrapBand(&op.text, 1, lines);
  const int y = drawBand(lines, renderer.getLineHeight(UI_12_FONT_ID) + 8) + 4;
  if (op.code == OpCode::Pick) {
    // Only the band changes; the squares are left as the test drew them.
    char line[64];
    snprintf(line, sizeof(line), "Tap a square, or < > then OK: %d", pickIndex + 1);
    renderer.drawCenteredText(UI_12_FONT_ID, y, line);
  } else {
    char left[48];
    char right[48];
    snprintf(left, sizeof(left), "< %s", op.options[0].c_str());
    snprintf(right, sizeof(right), "%s >", op.options[1].c_str());
    renderer.drawText(UI_12_FONT_ID, 16, y, left, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, w - 16 - renderer.getTextWidth(UI_12_FONT_ID, right, EpdFontFamily::BOLD), y,
                      right, true, EpdFontFamily::BOLD);
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  renderer.waitRefreshComplete();
  askDrawn = true;
}

void DisplayTestActivity::drawResult() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  TouchHeaderBackButton::draw(renderer, header, title.c_str(), false);
  // Goodies text pages: the list rows' font and label margin.
  const int font = uiScaleSpec().bodyFontId;
  const int x = metrics.listInset + metrics.listSidePadding;
  const int maxWidth = renderer.getScreenWidth() - 2 * x;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  const int lineHeight = renderer.getLineHeight(font) + 6;
  int y = header.y + header.height + metrics.verticalSpacing;
  // Word-wrapped to the width, a hanging indent on continuation lines.
  auto drawWrapped = [&](const char* text, const EpdFontFamily::Style style) {
    const auto lines = renderer.wrappedText(font, text, maxWidth, 3, style);
    for (size_t i = 0; i < lines.size() && y + lineHeight <= bottom; ++i) {
      renderer.drawText(font, i == 0 ? x : x + 16, y, lines[i].c_str(), true, style);
      y += lineHeight;
    }
  };
  char line[128];

  if (script.error) {
    snprintf(line, sizeof(line), "%s %d: %s", tr(STR_ERROR_MSG), script.errorLine, script.error);
  } else {
    snprintf(line, sizeof(line), "%s  %s: %d", stopped ? tr(STR_TEST_STOPPED) : tr(STR_DONE), tr(STR_REFRESHES),
             refreshCount);
  }
  drawWrapped(line, EpdFontFamily::BOLD);
  y += lineHeight / 2;

  // Average ms per mode: upload / drf / sync / total.
  for (int m = 0; m < 4; ++m) {
    const ModeStats& s = stats[m];
    if (s.count == 0) continue;
    snprintf(line, sizeof(line), "%-4s x%-3u %4lu / %4lu / %4lu / %4lu ms",
             display_script::modeName(static_cast<Mode>(m)), static_cast<unsigned>(s.count),
             static_cast<unsigned long>(s.upload / s.count), static_cast<unsigned long>(s.drf / s.count),
             static_cast<unsigned long>(s.sync / s.count), static_cast<unsigned long>(s.total / s.count));
    drawWrapped(line, EpdFontFamily::REGULAR);
  }
  if (refreshCount > 0) {
    drawWrapped("upload / drf / sync / total (avg)", EpdFontFamily::ITALIC);
    y += lineHeight / 2;
  }
  for (const auto& a : answers) drawWrapped(a.c_str(), EpdFontFamily::REGULAR);

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  // Half clears whatever the test left on the panel.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

#endif  // CROSSDINK_GOODIES
