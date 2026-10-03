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
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BatteryEstimate.h"
#include "util/BatteryLog.h"
#include "util/BootReason.h"
#include "util/BuildInfo.h"

namespace {

// Pointer to field n (0-based) of a CSV row, or nullptr.
const char* field(const char* row, int n) {
  while (n-- > 0) {
    row = strchr(row, ',');
    if (!row) return nullptr;
    ++row;
  }
  return row;
}

// A logged % is whole ("71", older rows and rows logged asleep) or has the
// CW2017 fraction ("71.43"), so a drop is exact to ±1 or ±0.01: rates and
// estimates carry that ±, and wait for a 2% drop (0.2% when mostly fractional).
uint32_t minDropC(const uint32_t dropC, const uint32_t coarseC) { return coarseC * 2 >= dropC ? 200 : 20; }
constexpr char NOT_ENOUGH[] = "not enough data";

// "71.43" -> 7143; fine = the field had a fraction.
uint16_t parseCenti(const char* field, bool& fine) {
  char* end = nullptr;
  const float v = strtof(field, &end);
  fine = end && memchr(field, '.', end - field) != nullptr;
  return static_cast<uint16_t>(std::clamp(lroundf(v * 100), 0L, 10000L));
}

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

// "4.12±0.20%/h over 5h 10m". errC is the ± squared (0.01 %², see LogStats).
// A ± past the rate shows as a range from 0 (drain is never negative);
// perDay adds the rate per day ("0.04 (0-0.15)%/h, ~1.0%/day over 2d 3h").
void formatRate(char* out, const size_t size, const uint32_t dropC, const uint32_t coarseC, const uint32_t errC,
                const uint32_t seconds, const bool perDay = false) {
  if (dropC < minDropC(dropC, coarseC) || seconds < 60) {
    snprintf(out, size, "%s", NOT_ENOUGH);
    return;
  }
  char span[24], day[24] = "";
  formatDur(seconds, span, sizeof(span));
  const float rate = dropC * 36.0f / seconds, err = sqrtf(static_cast<float>(errC)) * 36.0f / seconds;
  if (perDay) snprintf(day, sizeof(day), ", ~%.1f%%/day", rate * 24);
  if (err > rate) {
    snprintf(out, size, "%.2f (0-%.2f)%%/h%s over %s", rate, rate + err, day, span);
  } else {
    snprintf(out, size, "%.2f\xC2\xB1%.2f%%/h%s over %s", rate, err, day, span);
  }
}
}  // namespace

void BatteryStatsActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::flush();  // so the graph includes this session
  startLoad();          // the page draws at once; the log rows fill in when it is read
  requestUpdate();
}

void BatteryStatsActivity::onExit() {
  file.close();
  buf.reset();
  Activity::onExit();
}

// The log is read a few KB at a time from loop() so the page draws and takes
// input right away; up to 2 x 256 KB of CSV is too much to read in one go.
void BatteryStatsActivity::startLoad() {
  file.close();
  pointCount = 0;
  st = {};
  prev = {};
  fill = 0;
  fileIndex = 0;
  // Heap, not stack: 4 KB reads are multi-sector, far faster than 512 B ones.
  if (!buf) buf = makeUniqueNoThrow<char[]>(LOAD_BUF_BYTES);
  if (!buf) LOG_ERR("BAT", "Cannot allocate log buffer");
  if (buf) file = Storage.open(BatteryLog::OLD_PATH, O_RDONLY);  // only exists after the first rotation
  loading = buf != nullptr;
  loadStartMs = millis();
  loadBytes = 0;
  buildLines();
}

void BatteryStatsActivity::step(const uint32_t budgetMs) {
  const uint32_t start = millis();
  RenderLock lock(*this);  // render() reads points and lines
  while (loading && millis() - start < budgetMs) {
    const int n = file ? file.read(buf.get() + fill, LOAD_BUF_BYTES - 1 - fill) : 0;
    if (n <= 0) {
      file.close();
      fill = 0;  // a last row without a newline is still being written
      if (++fileIndex == 1) {
        file = Storage.open(BatteryLog::LOG_PATH, O_RDONLY);
        continue;
      }
      loading = false;
      buf.reset();
      endStretch(st);
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
      parseRow(line);
    }
    fill = strlen(line);
    memmove(buf.get(), line, fill);
    if (fill == LOAD_BUF_BYTES - 1) fill = 0;  // no row is this long; drop it
  }
}

void BatteryStatsActivity::endStretch(LogStats& s) {
  for (int k = 0; k < 2; ++k) {
    if (s.netC[k] > 0) {
      s.dropC[k] += static_cast<uint32_t>(s.netC[k]);
      s.coarseC[k] += static_cast<uint32_t>(std::clamp<int32_t>(s.netCoarseC[k], 0, s.netC[k]));
    }
    s.netC[k] = s.netCoarseC[k] = 0;
  }
  s.run = -1;
}

void BatteryStatsActivity::parseRow(const char* line) {
  const uint32_t epoch = strtoul(line, nullptr, 10);
  const char* pctField = field(line, 3);
  const char* chgField = field(line, 5);
  const char* usbField = field(line, 6);
  const char* event = field(line, 9);
  if (epoch == 0 || !pctField || !chgField || !usbField || !event) return;  // header, or no RTC time
  const char* detail = field(line, 10);
  auto is = [event](const char* name) {
    const size_t len = strlen(name);
    return strncmp(event, name, len) == 0 && (event[len] == ',' || event[len] == '\0');
  };
  bool fine = false;
  const uint16_t pctC = parseCenti(pctField, fine);
  const uint8_t pct = static_cast<uint8_t>(pctC / 100);
  const bool usb = *usbField == '1';
  const bool asleep = is("sleep") || (detail && strncmp(detail, "asleep", 6) == 0);
  const bool boot = is("boot");
  const bool cold = boot && detail && strstr(detail, "reset=POWERON");

  if (is("stats_reset")) {
    st = {};
    st.reset = true;
  } else if (st.first != 0 && prev.epoch != 0 && epoch >= prev.epoch && !cold) {
    // The span from the previous row is awake or asleep; before a cold boot it was off.
    const uint32_t dt = epoch - prev.epoch;
    const int32_t drop = static_cast<int32_t>(prevC) - pctC;  // negative: the gauge rose
    (prev.awake ? st.awakeS : st.asleepS) += dt;
    // Drops come from rows of one precision: a whole row (a charger event
    // logged asleep) inside fractional data counts its time, and the drop is
    // taken across it from prevC; the step from a whole row to a fractional
    // one is skipped (its rounding would be a drop of up to 1%).
    const int cat = prev.awake ? 0 : 1;
    if (prev.awake && !prevUsb && !usb && fine && prevRowFine &&
        (st.chargedEpoch == 0 || prev.epoch >= st.chargedEpoch + UNPLUG_SKIP_S)) {
      const int k = (prevWifi ? 2 : 0) + (prevLight ? 1 : 0);
      st.stateDropC[k] += static_cast<int32_t>(prevRowC) - pctC;
      st.stateS[k] += dt;
      st.stateLight[k] += static_cast<uint64_t>(prevLight) * dt;
    }
    if (!prevUsb && !usb && fine == prevFine) {
      st.battS[cat] += dt;
      st.netC[cat] += drop;
      if (!fine) st.netCoarseC[cat] += drop;
      if (st.run != cat || st.runFine != fine) st.errC[cat] += fine ? 1 : 10000;  // ± squared
      st.run = static_cast<int8_t>(cat);
      st.runFine = fine;
    } else if (!prevUsb && !usb && prevFine) {
      st.battS[cat] += dt;
    } else {
      endStretch(st);
    }
  } else {
    endStretch(st);
  }
  if (st.first == 0) st.first = epoch;
  st.last = epoch;
  if (boot) ++(cold ? st.coldBoots : st.restarts);
  if (is("wake")) ++st.wakes;
  if (const char* f = detail ? strstr(detail, "false_wakes=") : nullptr) st.falseWakes += strtoul(f + 12, nullptr, 10);
  if (usb || *chgField == '1') {
    if (!st.charging && (st.chargedEpoch == 0 || epoch - st.chargedEpoch >= CHARGE_MERGE_S)) {
      st.chargeFromC = pctC;
      st.chargeStartEpoch = epoch;
      st.chargeFromFine = fine;
    }
    st.charging = true;
  } else if (st.charging) {
    st.charging = false;
    st.chargedEpoch = epoch;
  }
  if (st.charging || st.chargedEpoch == epoch) {
    st.chargeToC = pctC;
    st.chargeToFine = fine;
  }

  if (pointCount == MAX_POINTS) {
    std::move(points + MAX_POINTS / 2, points + MAX_POINTS, points);
    pointCount = MAX_POINTS / 2;
  }
  prev = {epoch, pct, !asleep};
  // A whole row after fractional ones is not a drop reference, unless a USB
  // step, power-off gap or reset breaks the chain there.
  if (fine || !prevFine || usb || prevUsb || is("stats_reset") || cold) {
    prevC = pctC;
    prevFine = fine;
  }
  prevUsb = usb;
  prevRowC = pctC;
  prevRowFine = fine;
  if (boot || is("wake") || is("sleep") || is("wifi_off")) prevWifi = false;
  if (is("wifi_on")) prevWifi = true;
  const char* lightField = field(line, 8);
  prevLight = lightField ? static_cast<uint8_t>(atoi(lightField)) : 0;
  points[pointCount++] = prev;
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
  const uint16_t pct = powerManager.getBatteryPercentage();
  add("%u%%  %umV  %s  %s", pct, monitor.readMillivolts(), monitor.isCharging() ? "charging" : "",
      gpio.isUsbConnectedCached() ? "USB" : "on battery");
  if (tempKnown) {
    snprintf(lines[lineCount - 1] + strlen(lines[lineCount - 1]), sizeof(lines[0]) - strlen(lines[lineCount - 1]),
             "  %.1fC", tempDeci / 10.0f);
  }

  if (loading) {
    // Read from loop() in slices (step()); these fill in when it is done.
    for (const char* name : {"Chg", "Awake drain", "Asleep drain", "Est to empty"}) add("%s: calculating...", name);
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
      add("Chg: %s%% to %s%% over %s, %s ago", from, to, a, b);
    } else {
      add("Chg: not in the log");
    }
    formatRate(a, sizeof(a), st.dropC[0], st.coarseC[0], st.errC[0], st.battS[0]);
    add("Awake drain: %s", a);
    formatRate(a, sizeof(a), st.dropC[1], st.coarseC[1], st.errC[1], st.battS[1], true);
    add("Asleep drain: %s", a);
    // Awake drain for the live Wi-Fi and light state; the light's share scales
    // with brightness against the state's logged average.
    const bool wifiNow = WiFi.getMode() != WIFI_OFF;
    const uint8_t lightNow = Frontlight.present() && Frontlight.isOn() ? Frontlight.brightness() : 0;
    const int k = wifiNow ? 2 : 0;
    auto rateOf = [this](const int i) {  // 0.01 % per s, 0 = under 0.2% or a minute
      return st.stateS[i] >= 60 && st.stateDropC[i] >= 20 ? static_cast<float>(st.stateDropC[i]) / st.stateS[i] : 0.0f;
    };
    const float avgLight = st.stateS[k + 1] ? static_cast<float>(st.stateLight[k + 1]) / st.stateS[k + 1] : 0.0f;
    const float rate = BatteryEstimate::lightScaledRate(rateOf(k), rateOf(k + 1), avgLight, lightNow);
    const uint32_t pctNowC = powerManager.getBatteryPercent256() * 100u / 256u;
    const uint32_t drop = st.dropC[0] + st.dropC[1];
    const uint32_t span = st.battS[0] + st.battS[1];
    if (rate > 0) {
      formatDur(static_cast<uint32_t>(pctNowC / rate), a, sizeof(a));
      snprintf(b, sizeof(b), "%u%%", lightNow);
      add("Est to empty: %s (Wi-Fi %s, light %s)", a, wifiNow ? "on" : "off", lightNow ? b : "off");
    } else if (drop >= minDropC(drop, st.coarseC[0] + st.coarseC[1]) && span >= 60) {
      // The drop's ± moves the estimate by about left * ± / drop.
      const uint32_t left = static_cast<uint32_t>(static_cast<uint64_t>(pctNowC) * span / drop);
      char err[24];
      formatDur(left, a, sizeof(a));
      formatDur(static_cast<uint32_t>(left * sqrtf(static_cast<float>(st.errC[0] + st.errC[1])) / drop), err,
                sizeof(err));
      add("Est to empty: %s \xC2\xB1%s (avg)", a, err);
    } else {
      add("Est to empty: %s", NOT_ENOUGH);
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
    add("Light sleep %lu (%u%% of %s)  Rej %lu%s%s", static_cast<unsigned long>(ls.sleeps), ls.sleepPct, a,
        static_cast<unsigned long>(ls.rejects), ls.rejectCause ? ", last " : "",
        ls.rejectCause ? ls.rejectCauseName : "");
  }
  const auto& c = HalDisplay::refreshCounts().n;
  add("Session refresh counts: Fast %lu  Half %lu  Full %lu  Gray %lu  Flash %lu",
      static_cast<unsigned long>(c[HalDisplay::FAST_REFRESH]), static_cast<unsigned long>(c[HalDisplay::HALF_REFRESH]),
      static_cast<unsigned long>(c[HalDisplay::FULL_REFRESH]), static_cast<unsigned long>(c[HalDisplay::GRAY_PASSES]),
      static_cast<unsigned long>(c[HalDisplay::FLASHING]));

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

void BatteryStatsActivity::confirmReset() {
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_RESET) + std::string("?"),
                                                        tr(STR_BATTERY_STATS));
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
  if (mappedInput.hasTouchHardware()) {
    const Rect r = resetRect();
    if (mappedInput.wasTapInRect(r.x, r.y, r.width, r.height)) {
      confirmReset();
      return;
    }
  }
  if (loading) step(LOAD_STEP_MS);
  const auto swipe = mappedInput.wasSwipe();
  const bool down =
      mappedInput.wasReleased(MappedInputManager::Button::Down) || swipe == MappedInputManager::SwipeDir::Up;
  const bool up =
      mappedInput.wasReleased(MappedInputManager::Button::Up) || swipe == MappedInputManager::SwipeDir::Down;
  if ((down && more) || (up && scroll > 0)) {
    scroll += down && more ? 1 : -1;
    requestUpdate();
  }
}

void BatteryStatsActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    const Rect r = resetRect();
    TouchHeaderBackButton::draw(renderer, header, tr(STR_BATTERY_STATS), false, r.width);
    const auto l = TouchHeaderBackButton::layout(header);
    renderer.drawText(UI_10_FONT_ID, r.x + 12,
                      l.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                          (l.iconRect.height - renderer.getLineHeight(UI_10_FONT_ID)) / 2,
                      tr(STR_RESET));
  } else {
    GUI.drawHeader(renderer, header, tr(STR_BATTERY_STATS));
  }
  // Goodies text pages: the list rows' font and label margin.
  const int font = uiScaleSpec().bodyFontId;
  const int x = metrics.listInset + metrics.listSidePadding;
  const int w = renderer.getScreenWidth() - 2 * x;
  const int lineHeight = renderer.getLineHeight(font) + 6;
  int y = header.y + header.height + metrics.verticalSpacing;

  // Graph: % over time, 25% gridlines; a bar under it marks awake spans.
  const int gh = renderer.getScreenHeight() / 5;
  renderer.drawRect(x, y, w, gh);
  for (int q = 1; q < 4; ++q) {
    const int gy = y + gh * q / 4;
    for (int gx = x; gx < x + w; gx += 8) renderer.drawLine(gx, gy, gx + 2, gy);
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
      if (p0.awake) renderer.fillRect(px(p0.epoch), y + gh + 2, std::max(1, px(p1.epoch) - px(p0.epoch)), 4);
    }
    char spanText[40], ago[24];
    formatDur(spanS, ago, sizeof(ago));
    snprintf(spanText, sizeof(spanText), "%s  (bar = awake)", ago);
    renderer.drawText(font, x, y + gh + 8, spanText);
  } else {
    renderer.drawText(font, x + 8, y + gh / 2 - lineHeight / 2,
                      loading ? "Reading battery log..." : "No battery log yet");
  }
  y += gh + 8 + lineHeight + metrics.verticalSpacing;

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

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RESET), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSDINK_GOODIES && !SIMULATOR
