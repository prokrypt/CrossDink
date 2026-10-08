#include "PowerTestActivity.h"

#if CROSSDINK_GOODIES && !defined(SIMULATOR)

#include <BatteryMonitor.h>
#include <CrossDinkHalFrontlight.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <Knobs.h>
#include <Logging.h>
#include <PowerCounters.h>
#include <WiFi.h>
#include <esp_timer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>

#include "GoodiesActivity.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "util/LocalClock.h"
#include "util/PowerLog.h"

namespace {
struct Info {
  const char* label;
  const char* tag;
};
constexpr Info kTests[] = {
    {"Idle", "idle"},
    {"Light 50%", "light50"},
    {"Light 100%", "light100"},
    {"Wi-Fi idle (modem sleep)", "wifi_ps"},
    {"Wi-Fi awake", "wifi_awake"},
    {"CPU busy (one core)", "cpu"},
    {"Fast refresh loop", "fast_loop"},
    {"Full refresh loop", "full_loop"},
    {"Voltage sag probe", "sag"},
};
static_assert(sizeof(kTests) / sizeof(kTests[0]) == PowerTestActivity::TEST_COUNT, "one row per test");

constexpr const char* kLoadNames[] = {"light", "cpu", "wifi", "refresh"};
// Per load: settle after switching on, then the window the drop is averaged
// over. A Full refresh drives the panel for ~1.5 s, so its window starts at once.
constexpr uint16_t kApplyMs[] = {1500, 1500, 2500, 200};
constexpr uint16_t kWindowMs[] = {2000, 2000, 2000, 1200};
constexpr uint32_t kSettleMs = 4000;
constexpr uint32_t kBaseWindowMs = 2000;
constexpr uint32_t kSampleMs = 200;
constexpr uint32_t kJoinTimeoutMs = 30000;
constexpr uint32_t kSleepBlockPulseMs = 10000;

// 9 ms of spinning per tick on the worker core, at full clock. The 1-tick
// pause lets that core's idle task run, which the task watchdog checks.
std::atomic<bool> spinStop{false};
void spinMain(void*) {
  powerManager.beginBackgroundWork();
  while (!spinStop.load(std::memory_order_relaxed)) {
    const int64_t until = esp_timer_get_time() + 9000;
    while (esp_timer_get_time() < until) {
    }
    vTaskDelay(1);
  }
  powerManager.endBackgroundWork();
}

const BatteryMonitor& monitor() {
  static const BatteryMonitor m;  // built on first use, after BoardConfig::ACTIVE is set
  return m;
}

// 7143 in 1/100 % from 1/256 %: "71.43".
void formatPct256(char* out, const size_t size, const uint16_t p256) {
  snprintf(out, size, "%u.%02u", static_cast<unsigned>(p256 >> 8), static_cast<unsigned>((p256 & 0xFF) * 100 / 256));
}

void formatMin(char* out, const size_t size, const uint64_t ms) {
  const unsigned long m = static_cast<unsigned long>(ms / 60000);
  if (m >= 60) {
    snprintf(out, size, "%luh %lum", m / 60, m % 60);
  } else {
    snprintf(out, size, "%lum", m);
  }
}
}  // namespace

const char* PowerTestActivity::label(const int test) {
  return test >= 0 && test < TEST_COUNT ? kTests[test].label : "?";
}

const char* PowerTestActivity::tag(const int test) { return test >= 0 && test < TEST_COUNT ? kTests[test].tag : "?"; }

void PowerTestActivity::onEnter() {
  Activity::onEnter();
  goodies_remote::pause();  // the run owns the radio: off unless the test turns it on
  const uint8_t light = test == LIGHT_50 ? 50 : test == LIGHT_100 ? 100 : 0;
  Frontlight.setOverlay(light);
  if (test == WIFI_PS || test == WIFI_AWAKE) {
    if (joinWifi()) {
      phase = Phase::Joining;
      phaseMs = millis();
    } else {
      failure = "No saved Wi-Fi network";
      phase = Phase::Failed;
      Frontlight.setOverlay(HalFrontlight::NO_OVERLAY);
    }
  } else {
    radioOff();
    begin();
  }
  {
    RenderLock lock(*this);  // render() reads lines
    buildLines();
  }
  requestUpdate();
}

void PowerTestActivity::onExit() {
  if (phase == Phase::Running || phase == Phase::Joining) end(/*aborted=*/true);
  stopSpin();
  radioOff();
  Frontlight.setOverlay(HalFrontlight::NO_OVERLAY);
  Activity::onExit();
}

bool PowerTestActivity::joinWifi() {
  auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) cred = WIFI_STORE.getCredentialAt(0);
  if (!cred) {
    LOG_ERR("PWT", "No saved Wi-Fi network for the run");
    return false;
  }
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("PWT", "Station mode failed");
    return false;
  }
  wifiOn = true;
  WiFi.setSleep(false);  // awake for the join and DHCP; begin() sets the test's mode
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
  LOG_INF("PWT", "Joining %s", cred->ssid.c_str());
  return true;
}

void PowerTestActivity::radioOff() {
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    wifiOn = false;
    return;
  }
  WiFi.disconnect(false);
  WiFi.setSleep(true);  // as Goodies' remote: Arduino carries the PS mode into the next session
  WiFi.mode(WIFI_OFF);
  wifiOn = false;
}

void PowerTestActivity::startSpin() {
  if (spin.running()) return;
  spinStop.store(false, std::memory_order_relaxed);
  if (!spin.start(&spinMain, nullptr, 2048, "pwrspin")) LOG_ERR("PWT", "Spin task did not start");
}

void PowerTestActivity::stopSpin() {
  spinStop.store(true, std::memory_order_relaxed);
  if (spin.running()) spin.join();
}

uint16_t PowerTestActivity::readMv() { return monitor().readMillivolts(); }

PowerTestActivity::Snapshot PowerTestActivity::snapshot() const {
  const PowerCounters::Totals t = PowerCounters::totals();
  Snapshot s{};
  s.awakeMs = t.awakeMs;
  s.lsMs = t.lightSleepUs / 1000;
  s.maxMs = t.maxClockUs / 1000;
  s.lightFullMs = t.lightDutyMs / 1023;
  for (int i = PowerCounters::RADIO_UP; i < PowerCounters::RADIO_STATES; ++i) s.wifiMs += t.wifiMs[i];
  const auto& n = HalDisplay::refreshCounts().n;
  s.refreshes = n[HalDisplay::FULL_REFRESH] + n[HalDisplay::HALF_REFRESH] + n[HalDisplay::FAST_REFRESH] +
                n[HalDisplay::GRAY_PASSES];
  return s;
}

void PowerTestActivity::begin() {
  phase = Phase::Running;
  phaseMs = lastLoopMs = lastPulseMs = millis();
  runMs = test == SAG_PROBE ? 0 : static_cast<uint32_t>(KNOBS.powerTestMin) * 60000u;
  startPct256 = powerManager.getBatteryPercent256();
  startMv = readMv();
  start = snapshot();
  if (test == CPU_BUSY) startSpin();
  if (test == WIFI_PS || test == WIFI_AWAKE) WiFi.setSleep(test == WIFI_PS);
  if (test == SAG_PROBE) {
    sag = Sag::Settle;
    sagMs = millis();
  }
  PowerLog::event("test_start", tag(test));
  LOG_INF("PWT", "%s: running %lu min", label(test), static_cast<unsigned long>(runMs / 60000));
}

void PowerTestActivity::end(const bool aborted) {
  const bool started = phase == Phase::Running;
  phase = Phase::Done;
  endedMs = millis();
  if (started) {
    endPct256 = powerManager.getBatteryPercent256();
    endMv = readMv();
    endSnap = snapshot();
  }
  sleepBlockPulse = false;
  stopSpin();
  if (test == SAG_PROBE) {
    for (uint8_t l = 0; l < LOADS; ++l) setLoad(static_cast<Load>(l), false);
  }
  if (started) {
    PowerLog::event(aborted ? "test_abort" : "test_end", tag(test));
    PowerLog::flush();
  }
  radioOff();
  Frontlight.setOverlay(HalFrontlight::NO_OVERLAY);
  LOG_INF("PWT", "%s: %s after %lu s", label(test), aborted ? "stopped" : "done",
          static_cast<unsigned long>((endedMs - phaseMs) / 1000));
}

void PowerTestActivity::setLoad(const Load load, const bool on) {
  switch (load) {
    case LOAD_LIGHT:
      Frontlight.setOverlay(on ? 100 : 0);
      break;
    case LOAD_CPU:
      on ? startSpin() : stopSpin();
      break;
    case LOAD_WIFI:
      if (on) {
        WiFi.persistent(false);
        wifiOn = WiFi.mode(WIFI_STA);
        WiFi.setSleep(false);  // receiver on: the radio's idle draw, no association
      } else {
        radioOff();
      }
      break;
    case LOAD_REFRESH:
      if (on) {
        fullNext = true;
        requestUpdate();
      }
      break;
    default:
      break;
  }
}

void PowerTestActivity::sagStep() {
  const uint32_t now = millis();
  const bool measuring = sag == Sag::Base || sag == Sag::Load;
  if (measuring && now - lastSampleMs >= kSampleMs) {
    lastSampleMs = now;
    const uint16_t mv = readMv();
    if (mv != 0) {
      sampleSum += mv;
      ++sampleCount;
      windowSq += static_cast<double>(mv) * mv;
    }
  }
  switch (sag) {
    case Sag::Settle:
      if (now - sagMs < kSettleMs) return;
      sag = Sag::Base;
      break;
    case Sag::Base:
    case Sag::Load: {
      const uint32_t window = sag == Sag::Base ? kBaseWindowMs : kWindowMs[sagLoad];
      if (now - sagMs < window) return;
      if (sampleCount == 0) {
        failure = "The gauge gave no voltage";
        end(/*aborted=*/true);
        phase = Phase::Failed;
        RenderLock lock(*this);
        buildLines();
        lock.unlock();
        requestUpdate();
        return;
      }
      const double mean = static_cast<double>(sampleSum) / sampleCount;
      if (sag == Sag::Base) {
        baseMv = static_cast<float>(mean);
        // Variance of single reads within the window: the gauge's own noise.
        noiseSq += std::max(0.0, windowSq / sampleCount - mean * mean);
        ++noiseN;
        setLoad(static_cast<Load>(sagLoad), true);
        sag = Sag::Apply;
      } else {
        const float drop = static_cast<float>(mean) - baseMv;
        dropSum[sagLoad] += drop;
        dropSq[sagLoad] += drop * drop;
        ++dropN[sagLoad];
        setLoad(static_cast<Load>(sagLoad), false);
        sag = Sag::Next;
      }
      break;
    }
    case Sag::Apply:
      if (now - sagMs < kApplyMs[sagLoad]) return;
      sag = Sag::Load;
      break;
    case Sag::Next:
      if (++sagCycle >= SAG_CYCLES) {
        const int n = dropN[sagLoad];
        const float mean = n ? dropSum[sagLoad] / n : 0;
        const float sd = n > 1 ? sqrtf(std::max(0.0f, (dropSq[sagLoad] - n * mean * mean) / (n - 1))) : 0;
        char detail[48];
        snprintf(detail, sizeof(detail), "%s dmv=%.1f sd=%.1f n=%d base=%.0f", kLoadNames[sagLoad], mean, sd, n,
                 baseMv);
        PowerLog::event("sag", detail);
        sagCycle = 0;
        if (++sagLoad >= LOADS) {
          end(/*aborted=*/false);
          RenderLock lock(*this);
          buildLines();
          lock.unlock();
          requestUpdate();
          return;
        }
      }
      sag = Sag::Settle;
      break;
  }
  sagMs = now;
  sampleSum = 0;
  sampleCount = 0;
  windowSq = 0;
  lastSampleMs = 0;
}

uint32_t PowerTestActivity::msUntilTimedWork() const {
  if (phase != Phase::Running) return UINT32_MAX;
  const uint32_t now = millis();
  const auto until = [now](const uint32_t since, const uint32_t ms) {
    return now - since >= ms ? 0 : ms - (now - since);
  };
  uint32_t ms = until(phaseMs, runMs);
  if (test == FAST_LOOP || test == FULL_LOOP) ms = std::min(ms, until(lastLoopMs, KNOBS.powerTestRefreshS * 1000u));
  return ms;
}

void PowerTestActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();  // onExit() logs test_abort for a run still going
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    {
      RenderLock lock(*this);
      buildLines();
    }
    requestUpdate();
  }
  const uint32_t now = millis();
  sleepBlockPulse = false;
  if ((phase == Phase::Running || phase == Phase::Joining) && now - lastPulseMs >= kSleepBlockPulseMs) {
    lastPulseMs = now;
    sleepBlockPulse = true;
  }
  if (phase == Phase::Joining) {
    if (WiFi.status() == WL_CONNECTED) {
      begin();
    } else if (now - phaseMs >= kJoinTimeoutMs) {
      failure = "Wi-Fi did not connect in 30 s";
      radioOff();
      Frontlight.setOverlay(HalFrontlight::NO_OVERLAY);
      phase = Phase::Failed;
    } else {
      return;
    }
    {
      RenderLock lock(*this);
      buildLines();
    }
    requestUpdate();
    return;
  }
  if (phase != Phase::Running) return;
  if (test == SAG_PROBE) {
    sagStep();
    return;
  }
  if ((test == FAST_LOOP || test == FULL_LOOP) && now - lastLoopMs >= KNOBS.powerTestRefreshS * 1000u) {
    lastLoopMs = now;
    pattern = !pattern;
    fullNext = test == FULL_LOOP;
    {
      RenderLock lock(*this);
      buildLines();
    }
    requestUpdate();
  }
  if (now - phaseMs >= runMs) {
    end(/*aborted=*/false);
    {
      RenderLock lock(*this);
      buildLines();
    }
    requestUpdate();
  }
}

void PowerTestActivity::buildLines() {
  lineCount = 0;
  auto add = [this](const char* fmt, auto... args) {
    if (lineCount < MAX_LINES) snprintf(lines[lineCount++], sizeof(lines[0]), fmt, args...);
  };
  char a[24], b[24], c[24];
  add("%s", label(test));
  const uint32_t now = millis();
  switch (phase) {
    case Phase::Joining:
      add("Joining Wi-Fi...");
      break;
    case Phase::Running: {
      if (test == SAG_PROBE) {
        add("Stepping %s, cycle %d of %d", kLoadNames[sagLoad < LOADS ? sagLoad : 0], sagCycle + 1, SAG_CYCLES);
      } else {
        formatMin(a, sizeof(a), now - phaseMs);
        formatMin(b, sizeof(b), runMs);
        add("Running %s of %s", a, b);
      }
      break;
    }
    case Phase::Done:
      formatMin(a, sizeof(a), endedMs - phaseMs);
      add("Done after %s", a);
      break;
    case Phase::Failed:
      add("Could not run: %s", failure ? failure : "?");
      break;
  }
  if ((phase == Phase::Running || phase == Phase::Joining) && (gpio.isUsbConnectedCached() || monitor().isCharging())) {
    add("On USB or charging: the battery is not draining, so this run measures nothing.");
  }
  if (phase == Phase::Joining || (phase == Phase::Failed && startPct256 == 0)) {
    add("Back leaves.");
    return;
  }
  if (halClock.isAvailable() && halClock.formatTime(c, sizeof(c), LocalClock::currentOffsetQ())) add("Now %s", c);

  const bool done = phase != Phase::Running;  // Done or Failed after a start: the values end() kept
  const uint16_t nowPct256 = done ? endPct256 : powerManager.getBatteryPercent256();
  const uint32_t elapsedMs = (phase == Phase::Running ? now : endedMs) - phaseMs;
  formatPct256(a, sizeof(a), startPct256);
  formatPct256(b, sizeof(b), nowPct256);
  const int dropC = (static_cast<int>(startPct256) - nowPct256) * 100 / 256;
  if (dropC >= 30 && elapsedMs >= 60000) {
    add("Battery %s%% -> %s%%, -%d.%02d%% (%.2f %%/h)", a, b, dropC / 100, dropC % 100,
        dropC / 100.0f * 3600000.0f / elapsedMs);
  } else {
    add("Battery %s%% -> %s%% (under 0.3%%: no rate yet)", a, b);
  }
  add("Cell %u mV -> %u mV", static_cast<unsigned>(startMv), static_cast<unsigned>(done ? endMv : readMv()));

  const Snapshot s = done ? endSnap : snapshot();
  const uint64_t awake = s.awakeMs - start.awakeMs;
  if (awake > 0) {
    formatMin(a, sizeof(a), awake);
    add("Awake %s: light sleep %u%%, full clock %u%%", a, static_cast<unsigned>((s.lsMs - start.lsMs) * 100 / awake),
        static_cast<unsigned>((s.maxMs - start.maxMs) * 100 / awake));
  }
  formatMin(a, sizeof(a), s.lightFullMs - start.lightFullMs);
  formatMin(b, sizeof(b), s.wifiMs - start.wifiMs);
  add("At full light %s, Wi-Fi up %s, refreshes %lu", a, b, static_cast<unsigned long>(s.refreshes - start.refreshes));

  if (test == SAG_PROBE) {
    for (int l = 0; l < LOADS; ++l) {
      if (dropN[l] == 0) continue;
      const float mean = dropSum[l] / dropN[l];
      const float sd =
          dropN[l] > 1 ? sqrtf(std::max(0.0f, (dropSq[l] - dropN[l] * mean * mean) / (dropN[l] - 1))) : 0.0f;
      add("%s: %+.1f mV (sd %.1f, %d steps)", kLoadNames[l], mean, sd, dropN[l]);
    }
    if (noiseN > 0) add("Read noise: %.1f mV sd", sqrt(noiseSq / noiseN));
  }
  if (phase == Phase::Running) {
    add("Confirm updates this page. Back stops the run.");
  } else {
    add("Rows are in %s", PowerLog::LOG_PATH);
  }
}

void PowerTestActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  TouchHeaderBackButton::draw(renderer, header, "Power Test", false);
  const int font = uiScaleSpec().bodyFontId;
  const int x = metrics.listInset + metrics.listSidePadding;
  const int w = renderer.getScreenWidth() - 2 * x;
  const int lineHeight = renderer.getLineHeight(font) + 6;
  int y = header.y + header.height + metrics.verticalSpacing;
  const int bottom = renderer.getScreenHeight() - metrics.buttonHintsHeight;
  for (int i = 0; i < lineCount; ++i) {
    const auto wrapped = renderer.wrappedText(font, lines[i], w, 2);
    if (y + static_cast<int>(wrapped.size()) * lineHeight > bottom) break;
    for (size_t j = 0; j < wrapped.size(); ++j) {
      renderer.drawText(font, j == 0 ? x : x + 16, y, wrapped[j].c_str());
      y += lineHeight;
    }
  }
  // Refresh loops: half the free area black, swapping sides each frame, so
  // every refresh drives a real change across the panel.
  if ((test == FAST_LOOP || test == FULL_LOOP) && phase == Phase::Running && bottom - y > 40) {
    const int top = y + metrics.verticalSpacing;
    const int h = bottom - top - metrics.verticalSpacing;
    renderer.fillRect(pattern ? x : x + w / 2, top, w / 2, h);
  }
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_DISPLAY_REFRESH), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  const bool full = fullNext;
  fullNext = false;
  renderer.displayBuffer(full ? HalDisplay::FULL_REFRESH : HalDisplay::FAST_REFRESH);
}

#endif  // CROSSDINK_GOODIES && !SIMULATOR
