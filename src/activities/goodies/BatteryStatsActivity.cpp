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
constexpr uint32_t GRAPH_TAIL_BYTES = 32 * 1024;  // ~450 rows

// Pointer to field n (0-based) of a CSV row, or nullptr.
const char* field(const char* row, int n) {
  while (n-- > 0) {
    row = strchr(row, ',');
    if (!row) return nullptr;
    ++row;
  }
  return row;
}

// "4.1 %/h over 5h 10 min", or "-" with no drain seen yet.
void formatRate(char* out, const size_t size, const uint32_t dropPct, const uint32_t seconds) {
  char span[24];
  BookReadingStats::formatDuration(seconds, span, sizeof(span));
  if (seconds < 60) {
    snprintf(out, size, "-");
    return;
  }
  const float rate = dropPct * 3600.0f / seconds;
  snprintf(out, size, "%.1f %%/h over %s", rate, span);
}
}  // namespace

void BatteryStatsActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::flush();  // so the graph includes this session
  loadGraph();
  buildLines();
  requestUpdate();
}

void BatteryStatsActivity::loadGraph() {
  pointCount = 0;
  HalFile file = Storage.open(BatteryLog::LOG_PATH, O_RDONLY);
  if (!file) return;
  const size_t size = file.fileSize();
  const bool partial = size > GRAPH_TAIL_BYTES;
  file.seekSet(partial ? size - GRAPH_TAIL_BYTES : 0);
  char buf[512];
  size_t fill = 0;
  bool skipLine = partial;  // the first line of a tail read starts mid-row
  for (;;) {
    const int n = file.read(buf + fill, sizeof(buf) - 1 - fill);
    if (n <= 0) break;
    fill += static_cast<size_t>(n);
    buf[fill] = '\0';
    char* line = buf;
    for (char* nl; (nl = strchr(line, '\n')) != nullptr; line = nl + 1) {
      *nl = '\0';
      if (skipLine) {
        skipLine = false;
        continue;
      }
      const uint32_t epoch = strtoul(line, nullptr, 10);
      const char* pct = field(line, 3);
      const char* event = field(line, 9);
      if (epoch == 0 || !pct || !event) continue;  // header, or no RTC time
      if (pointCount == MAX_POINTS) {
        std::move(points + MAX_POINTS / 2, points + MAX_POINTS, points);
        pointCount = MAX_POINTS / 2;
      }
      const char* detail = field(line, 10);
      const bool asleep = strncmp(event, "sleep", 5) == 0 || (detail && strncmp(detail, "asleep", 6) == 0);
      points[pointCount++] = {epoch, static_cast<uint8_t>(std::min(100L, strtol(pct, nullptr, 10))), !asleep};
    }
    fill = strlen(line);
    memmove(buf, line, fill);
    if (fill == sizeof(buf) - 1) fill = 0;  // no row is this long; drop it
  }
  file.close();
}

void BatteryStatsActivity::buildLines() {
  lineCount = 0;
  auto add = [this](const char* fmt, auto... args) {
    if (lineCount < MAX_LINES) snprintf(lines[lineCount++], sizeof(lines[0]), fmt, args...);
  };
  char a[40], b[40];
  const BatteryLog::Stats& s = BatteryLog::stats();

  static const BatteryMonitor monitor;
  int16_t tempDeci = 0;
  const bool tempKnown = monitor.readTemperatureDeciC(tempDeci);
  const uint16_t pct = powerManager.getBatteryPercentage();
  add("%u%%  %u mV  %s  %s", pct, monitor.readMillivolts(), monitor.isCharging() ? "charging" : "",
      gpio.isUsbConnectedCached() ? "USB" : "on battery");
  if (tempKnown) {
    snprintf(lines[lineCount - 1] + strlen(lines[lineCount - 1]), sizeof(lines[0]) - strlen(lines[lineCount - 1]),
             "  %.1f C", tempDeci / 10.0f);
  }

  const uint32_t now = BatteryLog::nowEpoch();
  if (s.chargedEpoch != 0 && now > s.chargedEpoch) {
    BookReadingStats::formatDuration(now - s.chargedEpoch, a, sizeof(a));
    add("Last charged %s ago at %u%%", a, s.chargedPct);
  } else {
    add("Last charged: not seen since reset");
  }
  formatRate(a, sizeof(a), s.dropAwakePct, s.battAwakeS);
  add("Awake drain: %s", a);
  formatRate(a, sizeof(a), s.dropAsleepPct, s.battAsleepS);
  add("Asleep drain: %s", a);
  const uint32_t drop = s.dropAwakePct + s.dropAsleepPct;
  const uint32_t span = s.battAwakeS + s.battAsleepS;
  if (drop > 0 && span >= 60) {
    BookReadingStats::formatDuration(static_cast<uint32_t>(static_cast<uint64_t>(pct) * span / drop), a, sizeof(a));
    add("Est. left at that pace: %s", a);
  }

  BookReadingStats::formatDuration(s.awakeS, a, sizeof(a));
  BookReadingStats::formatDuration(s.asleepS, b, sizeof(b));
  add("Wakes %lu  False %lu  Boots %lu  Awake %s  Asleep %s", static_cast<unsigned long>(s.wakes),
      static_cast<unsigned long>(s.falseWakes + s.pendingFalseWakes), static_cast<unsigned long>(s.boots), a, b);
  const auto& c = HalDisplay::refreshCounts().n;
  add("Refresh: Fast %lu  Half %lu  Full %lu  Gray %lu  Flash %lu",
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
    add("Chip %.0f C  Panel %d C", chipC, panelC);
  } else {
    add("Chip %.0f C", chipC);
  }
  BookReadingStats::formatDuration(millis() / 1000, a, sizeof(a));
  add("Up %s  reset %s  wake %s", a, resetReasonName(esp_reset_reason()),
      wakeupCauseName(esp_sleep_get_wakeup_cause()));
  add("Heap %lu KB (block %lu)  PSRAM %lu KB free", static_cast<unsigned long>(ESP.getFreeHeap() / 1024),
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
    RenderLock lock(*this);  // render() reads lines
    BatteryLog::reset();
    buildLines();
    scroll = 0;
    lock.unlock();
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
    renderer.drawText(UI_10_FONT_ID, x + 8, y + gh / 2 - lineHeight / 2, "No battery log yet");
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
