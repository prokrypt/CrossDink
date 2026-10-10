#include "BatteryStatsActivity.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <BatteryMonitor.h>
#include <CrossDinkHalFrontlight.h>
#include <FreeInkDisplay.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Knobs.h>
#include <Memory.h>
#include <PerfLog.h>
#include <WiFi.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/home/BookActions.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchActionButtons.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BatteryEstimate.h"
#include "util/BatteryLog.h"
#include "util/BatteryLogSum.h"
#include "util/BootReason.h"
#include "util/BuildInfo.h"

namespace {

// Drops count between fractional rows only ("71.43"); rates and estimates carry
// the gauge's ± (see LogStats) and wait for a 0.2% drop.
constexpr uint32_t MIN_DROP_C = 20;
constexpr char NOT_ENOUGH[] = "not enough data";

// "12m", "1h 21m", "2d 3h".
void formatDur(const uint32_t seconds, char* out, const size_t size) {
  const unsigned long d = seconds / 86400, h = seconds % 86400 / 3600, m = seconds % 3600 / 60;
  if (d != 0) {
    snprintf(out, size, "%lud %luh", d, h);
  } else if (h != 0) {
    snprintf(out, size, "%luh %lum", h, m);
  } else {
    snprintf(out, size, "%lum", m);
  }
}

// 7143 -> "71.43" (fine) or "71".
void formatPct(char* out, const size_t size, const uint16_t centi, const bool fine) {
  if (fine) {
    snprintf(out, size, "%u.%02u", centi / 100, centi % 100);
  } else {
    snprintf(out, size, "%u", centi / 100);
  }
}

// "4.12 ±0.20%/h over 5h 10m". errC is the ± squared (0.01 %², BatteryLogParser::errSq).
// perDay shows only the rate per day ("0.99 ±0.20%/day over 6h 8m").
void formatRate(char* out, const size_t size, const uint32_t dropC, const float errC, const uint32_t seconds,
                const bool perDay = false) {
  if (dropC < MIN_DROP_C || seconds < 60) {
    snprintf(out, size, "%s", NOT_ENOUGH);
    return;
  }
  char span[24];
  formatDur(seconds, span, sizeof(span));
  const float unit = perDay ? 24.0f : 1.0f;
  const char* per = perDay ? "day" : "h";
  const float rate = dropC * 36.0f / seconds * unit, err = sqrtf(errC) * 36.0f / seconds * unit;
  snprintf(out, size, "%.2f \xC2\xB1%.2f%%/%s / %s", rate, err, per, span);
}

constexpr uint32_t SUM_MIN_READ = 32 * 1024;  // a load that read less leaves battery.sum as it is

// The live state the estimate is for: brightness (0 = off), +0x100 with Wi-Fi on.
uint16_t estimateState() {
  const uint8_t light = Frontlight.present() && Frontlight.isOn() ? Frontlight.brightness() : 0;
  return static_cast<uint16_t>(light | (WiFi.getMode() != WIFI_OFF ? 0x100 : 0));
}
}  // namespace

void BatteryStatsActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::flush();  // so the graph includes this session
  startLoad();
  // Rows since battery.sum usually read in a few ms, so the first frame has the
  // numbers; otherwise the page draws now and they fill in as loop() reads on.
  step(LOAD_STEP_MS * 2);
  requestUpdate();
}

void BatteryStatsActivity::onExit() {
  file.close();
  buf.reset();
  Activity::onExit();
}

// The log is read a few KB at a time from loop() so the page draws and takes
// input right away; a full read of up to 4 x 256 KB of CSV is too much for one go.
// Usually battery.sum leaves only the rows since the last load to read.
void BatteryStatsActivity::startLoad() {
  file.close();
  fill = 0;
  sumFile = -1;
  // Heap, not stack: 4 KB reads are multi-sector, far faster than 512 B ones.
  if (!buf) buf = makeUniqueNoThrow<char[]>(LOAD_BUF_BYTES);
  if (!buf) LOG_ERR("BAT", "Cannot allocate log buffer");
  if (buf && !resumeFromSum()) {
    parser = {};
    parser.stateSkipS = KNOBS.batteryStateSkipS;
    parser.halfLifeH = KNOBS.batteryHalfLifeH;
    fileIndex = BatteryLog::LOG_FILES - 1;  // the oldest; missing files read as empty
    fileOff = 0;
    file = Storage.open(BatteryLog::LOG_PATHS[fileIndex], O_RDONLY);
  }
  loading = buf != nullptr;
  loadStartMs = millis();
  loadBytes = 0;
  buildLines();
}

bool BatteryStatsActivity::resumeFromSum() {
  int i = 0;
  uint32_t off = 0;
  if (!BatteryLogSum::load(parser, i, off)) return false;
  file = Storage.open(BatteryLog::LOG_PATHS[i], O_RDONLY);
  if (!file || !file.seekSet(off)) {
    file.close();
    return false;
  }
  fileIndex = sumFile = i;
  fileOff = sumOff = off;
  LOG_INF("BAT", "Resuming the log at %s:%lu", BatteryLog::LOG_PATHS[i], static_cast<unsigned long>(off));
  return true;
}

void BatteryStatsActivity::step(const uint32_t budgetMs) {
  const uint32_t start = millis();
  RenderLock lock(*this);  // render() reads points and lines
  while (loading && millis() - start < budgetMs) {
    const int n = file ? file.read(buf.get() + fill, LOAD_BUF_BYTES - 1 - fill) : 0;
    if (n <= 0) {
      file.close();
      fill = 0;  // a last row without a newline is still being written
      if (fileOff != 0) {
        sumFile = fileIndex;
        sumOff = fileOff;
      }
      if (fileIndex > 0) {
        file = Storage.open(BatteryLog::LOG_PATHS[--fileIndex], O_RDONLY);
        fileOff = 0;
        continue;
      }
      loading = false;
      buf.reset();
      if (loadBytes > SUM_MIN_READ && sumFile >= 0) BatteryLogSum::save(parser, sumFile, sumOff);
      BatteryLogParser::endStretch(parser.st);
      LOG_INF("BAT", "Stats page read %lu B of log in %lu ms", static_cast<unsigned long>(loadBytes),
              static_cast<unsigned long>(millis() - loadStartMs));
      buildLines();
      lock.unlock();
      requestUpdate();
      return;
    }
    fill += static_cast<size_t>(n);
    loadBytes += static_cast<uint32_t>(n);
    buf[fill] = '\0';
    char* line = buf.get();
    for (char* nl; (nl = strchr(line, '\n')) != nullptr; line = nl + 1) {
      *nl = '\0';
      parser.parseRow(line);
    }
    const size_t rest = strlen(line);
    fileOff += static_cast<uint32_t>(fill - rest);
    fill = rest;
    memmove(buf.get(), line, fill);
    if (fill == LOAD_BUF_BYTES - 1) {  // no row is this long; drop it
      fileOff += static_cast<uint32_t>(fill);
      fill = 0;
    }
  }
}

void BatteryStatsActivity::buildLines() {
  lineCount = 0;
  auto add = [this](const char* fmt, auto... args) {
    if (lineCount < MAX_LINES) snprintf(lines[lineCount++], sizeof(lines[0]), fmt, args...);
  };
  char a[64], b[40];

  static const BatteryMonitor monitor;
  int16_t tempDeci = 0;
  const bool tempKnown = monitor.readTemperatureDeciC(tempDeci);
  const uint32_t pctC = powerManager.getBatteryPercent256() * 100u / 256u;
  add("%u.%02u%%  %umV  %s  %s", static_cast<unsigned>(pctC / 100), static_cast<unsigned>(pctC % 100),
      monitor.readMillivolts(), monitor.isCharging() ? "charging" : "",
      gpio.isUsbConnectedCached() ? "USB" : "on battery");
  if (tempKnown) {
    snprintf(lines[lineCount - 1] + strlen(lines[lineCount - 1]), sizeof(lines[0]) - strlen(lines[lineCount - 1]),
             "  %.1fC", tempDeci / 10.0f);
  }

  const auto& st = parser.st;
  if (loading) {
    // Read from loop() in slices (step()); these fill in when it is done.
    for (const char* name : {"Chg", "Awake drain", "Asleep", "To empty"}) add("%s: calculating...", name);
  } else {
    const uint32_t now = BatteryLog::nowEpoch();
    char from[8], to[8];
    formatPct(from, sizeof(from), st.chargeFromC, st.chargeFromFine);
    formatPct(to, sizeof(to), st.chargeToC, st.chargeToFine);
    if (st.charging) {
      add("Charging from %s%% (now %s%%)", from, to);
    } else if (st.chargedEpoch != 0 && now > st.chargedEpoch) {
      formatDur(st.chargedEpoch - st.chargeStartEpoch, a, sizeof(a));
      formatDur(now - st.chargedEpoch, b, sizeof(b));
      add("Chg: %s%% to %s%% / %s, %s ago", from, to, a, b);
    } else {
      add("Chg: not in the log");
    }
    formatRate(a, sizeof(a), st.dropC[0], BatteryLogParser::errSq(st, 0), st.battS[0]);
    add("Awake drain: %s", a);
    formatRate(a, sizeof(a), st.dropC[1], BatteryLogParser::errSq(st, 1), st.battS[1], true);
    add("Asleep: %s", a);
    // Awake drain for the live Wi-Fi and light state; the light's share scales
    // with the LED duty against the state's logged average duty.
    builtState = estimateState();
    const bool wifiNow = builtState >> 8;
    const uint8_t lightNow = builtState & 0xFF;
    // 0.01 % per s, 0 = under 0.5% or 30 min: one short stretch is mostly the gauge's wander.
    // Rate and duty are recency-weighted (BatteryLogParser::addRecent).
    auto rateOf = [&st](const int i) {
      return st.stateS[i] >= 1800 && st.stateDropC[i] >= 50 && st.recentS[i] > 0
                 ? std::max(st.recentDropC[i] / st.recentS[i], 0.0f)
                 : 0.0f;
    };
    auto dutyOf = [&st](const int i) { return st.recentS[i] > 0 ? st.recentDuty[i] / st.recentS[i] : 0.0f; };
    // The LED's full-duty drain (0.1 %/h) per duty unit, in 0.01 % per s.
    const float maxSlope = KNOBS.ledMaxDrain / 360.0f / 1023;
    auto rateFor = [&](const int k, const int o) {  // k: this Wi-Fi state, o: the other
      return BatteryEstimate::lightScaledRate(rateOf(k), rateOf(k + 1), dutyOf(k + 1), lightNow,
                                              BatteryEstimate::ledSlope(rateOf(o), rateOf(o + 1), dutyOf(o + 1)),
                                              maxSlope);
    };
    // Wi-Fi only adds drain, so with it on the estimate never beats Wi-Fi off.
    const float rate = wifiNow ? std::max(rateFor(2, 0), rateFor(0, 2)) : rateFor(0, 2);
    const uint32_t pctNowC = powerManager.getBatteryPercent256() * 100u / 256u;
    const uint32_t drop = st.dropC[0] + st.dropC[1];
    const uint32_t span = st.battS[0] + st.battS[1];
    char light[8];
    snprintf(light, sizeof(light), "%u%%", lightNow);
    snprintf(b, sizeof(b), "Wi-Fi %s, light %s", wifiNow ? "on" : "off", lightNow ? light : "off");
    if (rate > 0) {
      formatDur(static_cast<uint32_t>(pctNowC / rate), a, sizeof(a));
      add("To empty: %s awake (%s)", a, b);
    } else if (drop >= MIN_DROP_C && span >= 60) {
      // No awake rate: drop over the whole on-battery span, sleep included.
      // The drop's ± moves the estimate by about left * ± / drop.
      const uint32_t left = static_cast<uint32_t>(static_cast<uint64_t>(pctNowC) * span / drop);
      char err[24];
      formatDur(left, a, sizeof(a));
      formatDur(
          static_cast<uint32_t>(left * sqrtf(BatteryLogParser::errSq(st, 0) + BatteryLogParser::errSq(st, 1)) / drop),
          err, sizeof(err));
      add("To empty: %s \xC2\xB1%s calendar (%s)", a, err, b);
    } else {
      add("To empty: %s", NOT_ENOUGH);
    }

    if (st.first != 0 && now > st.first) {
      formatDur(now - st.first, a, sizeof(a));
      add("Earliest log: %s ago%s", a, st.reset ? " (reset)" : "");
    } else {
      add("Earliest log: none");
    }
    add("Wakes %lu  False %lu  Cold %lu  Restarts %lu", static_cast<unsigned long>(st.wakes),
        static_cast<unsigned long>(st.falseWakes), static_cast<unsigned long>(st.coldBoots),
        static_cast<unsigned long>(st.restarts));
    formatDur(st.awakeS, a, sizeof(a));
    formatDur(st.asleepS, b, sizeof(b));
    add("Awake %s  Asleep %s", a, b);
  }
  PerfLog::LightSleepStats ls;
  if (PerfLog::lightSleepStats(ls)) {
    formatDur(ls.upS, a, sizeof(a));
    add("LS %lu (%u%% of %s)  Rej %lu%s%s", static_cast<unsigned long>(ls.sleeps), ls.sleepPct, a,
        static_cast<unsigned long>(ls.rejects), ls.rejectCause ? ", last " : "",
        ls.rejectCause ? ls.rejectCauseName : "");
  }
  const auto& c = HalDisplay::refreshCounts().n;
  add("Ref: Fa %lu  Ha %lu  Fu %lu  Gr %lu  Fl %lu", static_cast<unsigned long>(c[HalDisplay::FAST_REFRESH]),
      static_cast<unsigned long>(c[HalDisplay::HALF_REFRESH]), static_cast<unsigned long>(c[HalDisplay::FULL_REFRESH]),
      static_cast<unsigned long>(c[HalDisplay::GRAY_PASSES]), static_cast<unsigned long>(c[HalDisplay::FLASHING]));

  int8_t panelC = 0;
  uint32_t panelAgeMs = 0;
  const float chipC = temperatureRead();
  if (freeink::uc8179PanelTemperature(panelC, panelAgeMs)) {
    add("CPU %.0fC  Panel %dC", chipC, panelC);
  } else {
    add("CPU %.0fC", chipC);
  }
  formatDur(millis() / 1000, a, sizeof(a));
  add("Up %s  reset %s  wake %s", a, resetReasonName(esp_reset_reason()),
      wakeupCauseName(esp_sleep_get_wakeup_cause()));
  add("%s  log %s", BuildInfo::gitSha(), BatteryLog::LOG_PATH);
}

Rect BatteryStatsActivity::resetRect() const {
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const auto l = TouchHeaderBackButton::layout(header);
  const int w = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_RESET)) + 24;
  return Rect{header.x + header.width - w, l.touchRect.y, w, l.touchRect.height};
}

Rect BatteryStatsActivity::refreshRect() const {
  const Rect reset = resetRect();
  const int w = renderer.getTextWidth(UI_10_FONT_ID, tr(STR_DISPLAY_REFRESH)) + 24;
  return Rect{reset.x - w, reset.y, w, reset.height};
}

// Re-reads the log and live readings, as on entering the page.
void BatteryStatsActivity::refresh() {
  BatteryLog::flush();
  {
    RenderLock lock(*this);  // render() reads points and lines
    startLoad();
  }
  // As onEnter(): one frame with the numbers, not one before and one after the read.
  step(LOAD_STEP_MS * 2);
  requestUpdate();
}

void BatteryStatsActivity::confirmReset() {
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(
      renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_RESET), tr(STR_BATTERY_STATS));
  if (!dialog) {
    LOG_ERR("BAT", "Cannot allocate reset dialog");
    return;
  }
  startActivityForResult(std::move(dialog), [this](const ActivityResult& res) {
    if (res.isCancelled) return;
    BatteryLog::event("stats_reset");
    BatteryLog::reset();
    BatteryLog::flush();
    {
      RenderLock lock(*this);  // render() reads points and lines
      startLoad();
      scroll = 0;
    }
    requestUpdate();
  });
}

void BatteryStatsActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    confirmReset();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    refresh();
    return;
  }
  if (mappedInput.hasTouchHardware()) {
    const Rect r = resetRect();
    if (mappedInput.wasTapInRect(r.x, r.y, r.width, r.height)) {
      confirmReset();
      return;
    }
    const Rect f = refreshRect();
    if (mappedInput.wasTapInRect(f.x, f.y, f.width, f.height)) {
      refresh();
      return;
    }
  }
  if (loading) step(LOAD_STEP_MS);
  // Brightness or Wi-Fi changed on this page: redo the estimate and repaint at once.
  if (!loading && estimateState() != builtState) {
    RenderLock lock(*this);  // render() reads lines
    buildLines();
    requestUpdate();
  }
  const auto swipe = mappedInput.wasSwipe();
  const bool down =
      mappedInput.wasReleased(MappedInputManager::Button::Down) || swipe == MappedInputManager::SwipeDir::Up;
  const bool up =
      mappedInput.wasReleased(MappedInputManager::Button::Up) || swipe == MappedInputManager::SwipeDir::Down;
  if ((down && more) || (up && scroll > 0)) {
    scroll += down && more ? 1 : -1;
    requestUpdate();
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Up) && !mappedInput.hasLeftRightButtonsHardware()) {
    refresh();  // X4 Pro has no Right: Up at the top refreshes
  }
}

void BatteryStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  const Rect r = resetRect();
  const Rect f = refreshRect();
  TouchHeaderBackButton::draw(renderer, header, tr(STR_BATTERY_STATS), false, r.width + f.width);
  if (mappedInput.hasTouchHardware()) {
    // Bordered text buttons (the Update screen's style) in the back icon's
    // band; the tap targets stay refreshRect() and resetRect().
    const auto l = TouchHeaderBackButton::layout(header);
    const int by = l.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                   (l.iconRect.height - TouchHeaderBackButton::ICON_SIZE) / 2;
    TouchActionButtons::Layout actions;
    actions.count = 2;
    actions.buttons[0] = Rect{f.x + 4, by, f.width - 8, TouchHeaderBackButton::ICON_SIZE};
    actions.buttons[1] = Rect{r.x + 4, by, r.width - 8, TouchHeaderBackButton::ICON_SIZE};
    const char* const actionLabels[] = {tr(STR_DISPLAY_REFRESH), tr(STR_RESET)};
    TouchActionButtons::draw(renderer, actions, actionLabels, -1, -1, UI_10_FONT_ID);
  }
  // Goodies text pages: the list rows' font and label margin.
  const int font = uiScaleSpec().bodyFontId;
  const int x = metrics.listInset + metrics.listSidePadding;
  const int w = renderer.getScreenWidth() - 2 * x;
  const int lineHeight = renderer.getLineHeight(font) + 6;
  int y = header.y + header.height + metrics.verticalSpacing;

  // Graph: % over time, 25% gridlines; two bars under it mark Wi-Fi and awake spans.
  const int gh = renderer.getScreenHeight() / 5;
  renderer.drawRect(x, y, w, gh);
  for (int q = 1; q < 4; ++q) {
    const int gy = y + gh * q / 4;
    for (int gx = x; gx < x + w; gx += 8) renderer.drawLine(gx, gy, gx + 2, gy);
  }
  const Point* points = parser.points;
  int pointCount = parser.pointCount;
  if (pointCount > 0) {  // only the newest GRAPH_S
    const uint32_t last = points[pointCount - 1].epoch;
    const uint32_t cut = last > BatteryLogParser::GRAPH_S ? last - BatteryLogParser::GRAPH_S : 0;
    int skip = 0;
    while (skip < pointCount && points[skip].epoch < cut) ++skip;
    points += skip;
    pointCount -= skip;
  }
  if (pointCount >= 2 && points[pointCount - 1].epoch > points[0].epoch) {
    const uint32_t t0 = points[0].epoch;
    const uint32_t spanS = points[pointCount - 1].epoch - t0;
    auto px = [&](const uint32_t t) { return x + static_cast<int>(static_cast<uint64_t>(t - t0) * (w - 1) / spanS); };
    auto py = [&](const uint8_t p) { return y + gh - 1 - p * (gh - 1) / 100; };
    for (int i = 1; i < pointCount; ++i) {
      const Point& p0 = points[i - 1];
      const Point& p1 = points[i];
      if (p1.epoch < p0.epoch) continue;  // clock set backwards
      renderer.drawLine(px(p0.epoch), py(p0.pct), px(p1.epoch), py(p1.pct), 2, true);
      const int bw = std::max(1, px(p1.epoch) - px(p0.epoch));
      // Ticks at a plug-in (bottom) and a `charged` row (top).
      if (p1.mark & BatteryLogParser::MARK_CHARGE) {
        renderer.drawLine(px(p1.epoch), y + gh - 14, px(p1.epoch), y + gh - 2, 2, true);
      }
      if (p1.mark & BatteryLogParser::MARK_CHARGED) {
        renderer.drawLine(px(p1.epoch), y + 2, px(p1.epoch), y + 14, 2, true);
      }
      if (p0.wifi) renderer.fillRect(px(p0.epoch), y + gh + 2, bw, 4);
      if (p0.awake) renderer.fillRect(px(p0.epoch), y + gh + 8, bw, 4);
    }
    char spanText[40], ago[24];
    formatDur(spanS, ago, sizeof(ago));
    snprintf(spanText, sizeof(spanText), "%s  (bars: Wi-Fi, awake)", ago);
    renderer.drawText(font, x, y + gh + 14, spanText);
  } else {
    renderer.drawText(font, x + 8, y + gh / 2 - lineHeight / 2,
                      loading ? "Reading battery log..." : "No battery log yet");
  }
  y += gh + 14 + lineHeight + metrics.verticalSpacing;

  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  // Lines that don't fit scroll with Up/Down or a swipe; "..." marks more below.
  more = false;
  for (int i = scroll; i < lineCount && !more; ++i) {
    const auto wrapped = renderer.wrappedText(font, lines[i], w, 2);
    if (y + static_cast<int>(wrapped.size()) * lineHeight > bottom) {
      more = true;
      renderer.drawText(font, x, y, "...");
      break;
    }
    for (size_t j = 0; j < wrapped.size(); ++j) {
      renderer.drawText(font, j == 0 ? x : x + 16, y, wrapped[j].c_str());
      y += lineHeight;
    }
  }

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RESET), "", tr(STR_DISPLAY_REFRESH));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSDINK_GOODIES && !SIMULATOR
