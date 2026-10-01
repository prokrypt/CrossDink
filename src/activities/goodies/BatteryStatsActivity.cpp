#include "BatteryStatsActivity.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <BatteryMonitor.h>
#include <FreeInkDisplay.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <esp_sleep.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
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

// The gauge reports whole percents, so a drop of N% means N±1: rates and
// estimates carry that ±, and wait for a 2% drop (1% could be 0 to 2).
constexpr uint32_t MIN_DROP_PCT = 2;
constexpr char NOT_ENOUGH[] = "not enough data";

// "4.1±0.2%/h over 5h 10 min".
void formatRate(char* out, const size_t size, const uint32_t dropPct, const uint32_t seconds) {
  if (dropPct < MIN_DROP_PCT || seconds < 60) {
    snprintf(out, size, "%s", NOT_ENOUGH);
    return;
  }
  char span[24];
  BookReadingStats::formatDuration(seconds, span, sizeof(span));
  snprintf(out, size, "%.1f\xC2\xB1%.1f%%/h over %s", dropPct * 3600.0f / seconds, 3600.0f / seconds, span);
}
}  // namespace

void BatteryStatsActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::flush();  // so the graph includes this session
  startLoad();
  step(LOAD_FIRST_MS);  // a short log is done before the first draw
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
  if (!buf) {
    LOG_ERR("BAT", "Cannot allocate log buffer");
    loading = false;
    buildLines();
    return;
  }
  file = Storage.open(BatteryLog::OLD_PATH, O_RDONLY);  // only exists after the first rotation
  loading = true;
  loadStartMs = millis();
  loadBytes = 0;
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

void BatteryStatsActivity::parseRow(const char* line) {
  const uint32_t epoch = strtoul(line, nullptr, 10);
  const char* pctField = field(line, 3);
  const char* usbField = field(line, 6);
  const char* event = field(line, 9);
  if (epoch == 0 || !pctField || !usbField || !event) return;  // header, or no RTC time
  const char* detail = field(line, 10);
  auto is = [event](const char* name) {
    const size_t len = strlen(name);
    return strncmp(event, name, len) == 0 && (event[len] == ',' || event[len] == '\0');
  };
  const uint8_t pct = static_cast<uint8_t>(std::min(100L, strtol(pctField, nullptr, 10)));
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
    const uint32_t drop = prev.pct > pct ? prev.pct - pct : 0;
    (prev.awake ? st.awakeS : st.asleepS) += dt;
    if (!prevUsb && !usb) {
      (prev.awake ? st.battAwakeS : st.battAsleepS) += dt;
      (prev.awake ? st.dropAwakePct : st.dropAsleepPct) += drop;
    }
  }
  if (st.first == 0) st.first = epoch;
  st.last = epoch;
  if (boot) ++(cold ? st.coldBoots : st.restarts);
  if (is("wake")) ++st.wakes;
  if (const char* f = detail ? strstr(detail, "false_wakes=") : nullptr) st.falseWakes += strtoul(f + 12, nullptr, 10);
  if (is("charged")) {
    st.chargedEpoch = epoch;
    st.chargedPct = pct;
  }
  if (is("charged") || is("chg_off")) st.battAwakeS = st.battAsleepS = st.dropAwakePct = st.dropAsleepPct = 0;

  if (pointCount == MAX_POINTS) {
    std::move(points + MAX_POINTS / 2, points + MAX_POINTS, points);
    pointCount = MAX_POINTS / 2;
  }
  prev = {epoch, pct, !asleep};
  prevUsb = usb;
  points[pointCount++] = prev;
}

void BatteryStatsActivity::buildLines() {
  lineCount = 0;
  auto add = [this](const char* fmt, auto... args) {
    if (lineCount < MAX_LINES) snprintf(lines[lineCount++], sizeof(lines[0]), fmt, args...);
  };
  char a[40], b[40];

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

  const uint32_t now = BatteryLog::nowEpoch();
  if (st.chargedEpoch != 0 && now > st.chargedEpoch) {
    BookReadingStats::formatDuration(now - st.chargedEpoch, a, sizeof(a));
    add("Last charged %s ago at %u%%", a, st.chargedPct);
  } else {
    add("Last charged: not in the log");
  }
  formatRate(a, sizeof(a), st.dropAwakePct, st.battAwakeS);
  add("Awake drain: %s", a);
  formatRate(a, sizeof(a), st.dropAsleepPct, st.battAsleepS);
  add("Asleep drain: %s", a);
  const uint32_t drop = st.dropAwakePct + st.dropAsleepPct;
  const uint32_t span = st.battAwakeS + st.battAsleepS;
  if (drop >= MIN_DROP_PCT && span >= 60) {
    // ±1% on the drop moves the estimate by about left/drop.
    const uint32_t left = static_cast<uint32_t>(static_cast<uint64_t>(pct) * span / drop);
    char err[24];
    BookReadingStats::formatDuration(left, a, sizeof(a));
    BookReadingStats::formatDuration(left / drop, err, sizeof(err));
    add("Est. left at that pace: %s \xC2\xB1%s", a, err);
  } else {
    add("Est. left: %s", NOT_ENOUGH);
  }

  BookReadingStats::formatDuration(st.last - st.first, a, sizeof(a));
  add("Log: %s%s", st.first ? a : "empty", st.reset ? " since reset" : "");
  add("Wakes %lu  False %lu  Cold boots %lu  Restarts %lu", static_cast<unsigned long>(st.wakes),
      static_cast<unsigned long>(st.falseWakes), static_cast<unsigned long>(st.coldBoots),
      static_cast<unsigned long>(st.restarts));
  BookReadingStats::formatDuration(st.awakeS, a, sizeof(a));
  BookReadingStats::formatDuration(st.asleepS, b, sizeof(b));
  add("Awake %s  Asleep %s", a, b);
  const auto& c = HalDisplay::refreshCounts().n;
  add("Refresh since power-on: Fast %lu  Half %lu  Full %lu  Gray %lu  Flash %lu",
      static_cast<unsigned long>(c[HalDisplay::FAST_REFRESH]), static_cast<unsigned long>(c[HalDisplay::HALF_REFRESH]),
      static_cast<unsigned long>(c[HalDisplay::FULL_REFRESH]), static_cast<unsigned long>(c[HalDisplay::GRAY_PASSES]),
      static_cast<unsigned long>(c[HalDisplay::FLASHING]));

  const GlobalReadingStats reading = GlobalReadingStats::load();
  BookReadingStats::formatDuration(reading.totalReadingSeconds, a, sizeof(a));
  add("Reading: %lu pages, %s", static_cast<unsigned long>(reading.totalPagesTurned), a);

  int8_t panelC = 0;
  uint32_t panelAgeMs = 0;
  const float chipC = temperatureRead();
  if (freeink::uc8179PanelTemperature(panelC, panelAgeMs)) {
    add("Chip %.0fC  Panel %dC", chipC, panelC);
  } else {
    add("Chip %.0fC", chipC);
  }
  BookReadingStats::formatDuration(millis() / 1000, a, sizeof(a));
  add("Up %s  reset %s  wake %s", a, resetReasonName(esp_reset_reason()),
      wakeupCauseName(esp_sleep_get_wakeup_cause()));
  add("Heap %luKB (block %lu)  PSRAM %luKB free", static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
      static_cast<unsigned long>(ESP.getMaxAllocHeap() / 1024), static_cast<unsigned long>(ESP.getFreePsram() / 1024));
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
  const int x = metrics.contentSidePadding;
  const int w = renderer.getScreenWidth() - 2 * x;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID) + 6;
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
    BookReadingStats::formatDuration(spanS, ago, sizeof(ago));
    snprintf(spanText, sizeof(spanText), "%s  (bar = awake)", ago);
    renderer.drawText(UI_10_FONT_ID, x, y + gh + 8, spanText);
  } else {
    renderer.drawText(UI_10_FONT_ID, x + 8, y + gh / 2 - lineHeight / 2,
                      loading ? "Reading battery log..." : "No battery log yet");
  }
  y += gh + 8 + lineHeight + metrics.verticalSpacing;

  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  // Lines that don't fit scroll with Up/Down or a swipe; "..." marks more below.
  more = false;
  for (int i = scroll; i < lineCount && !more; ++i) {
    const auto wrapped = renderer.wrappedText(UI_10_FONT_ID, lines[i], w, 2);
    if (y + static_cast<int>(wrapped.size()) * lineHeight > bottom) {
      more = true;
      renderer.drawText(UI_10_FONT_ID, x, y, "...");
      break;
    }
    for (size_t j = 0; j < wrapped.size(); ++j) {
      renderer.drawText(UI_10_FONT_ID, j == 0 ? x : x + 16, y, wrapped[j].c_str());
      y += lineHeight;
    }
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RESET), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSDINK_GOODIES && !SIMULATOR
