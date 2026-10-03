#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "BatteryEstimate.h"

// The Goodies > Battery & stats counters and graph points, counted from the
// battery log rows (BatteryLog.h) one row at a time. Plain data: the whole
// object is the page's saved parse checkpoint (battery.sum), so every bit of
// state carried from one row to the next lives in it.
struct BatteryLogParser {
  // Unbroken runs of on-battery steps in one category: count and the sums the
  // drop's ± is fitted from (errSq): d = drop (0.01 %), t = seconds.
  struct RunSums {
    uint32_t n;
    int64_t dt;
    uint64_t d2, t2;
  };
  struct Point {
    uint32_t epoch;
    uint8_t pct;
    bool awake;  // awake until the next row (not a sleep row or a charger row logged "asleep")
  };
  // Counted from the log since its first row or the last stats_reset row.
  struct LogStats {
    uint32_t first, last;  // epochs
    bool reset;            // first is a stats_reset row
    uint32_t coldBoots, restarts, wakes, falseWakes;
    uint32_t awakeS, asleepS;  // power-off gaps before a cold boot count as neither
    // Last charge session: rows with USB or charging, merged across gaps under
    // CHARGE_MERGE_S (USB flapping). chargedEpoch is its end, 0 = none yet.
    uint32_t chargedEpoch, chargeStartEpoch;
    uint16_t chargeFromC, chargeToC;  // 0.01 %
    bool chargeFromFine, chargeToFine, charging;
    // On battery, over the whole log; [0] awake, [1] asleep: seconds, drop in
    // 0.01 %, and its runs (the ± is their scatter, errSq).
    uint32_t battS[2], dropC[2];
    RunSums runs[2];
    int8_t run;    // category of the open run, -1 = none
    int32_t runC;  // the open run's drop and seconds
    uint32_t runS;
    // The open stretch of on-battery steps: net drop per category, signed, so
    // the gauge's rise after an unplug cancels drops instead of being ignored,
    // and its runs. Added to dropC and runs when the stretch ends, unless the
    // net is a rise (then neither counts).
    int32_t netC[2];
    RunSums netRuns[2];
    // Awake on battery, between fractional rows, by state [Wi-Fi * 2 + light on]:
    // signed drop in 0.01 %, seconds, and LED duty (BatteryEstimate::lightDuty)
    // x seconds. The first UNPLUG_SKIP_S after a charge is left out (the gauge
    // rises then), and steps starting under stateSkipS after a light, Wi-Fi,
    // wake or boot change (the gauge sags or recovers with the load).
    int32_t stateDropC[4];
    uint32_t stateS[4];
    uint64_t stateDuty[4];
  };
  static constexpr uint32_t CHARGE_MERGE_S = 60;
  static constexpr uint32_t UNPLUG_SKIP_S = 1800;
  static constexpr int MAX_POINTS = 400;
  static constexpr uint32_t RUN_ERR_C = 625;  // (0.25 %)² per run while there are too few runs to fit
  static constexpr uint16_t FULL_C = 9500;    // a charge ending at or above this may still be on a charger
  static constexpr uint16_t FULL_DROP_C = 5;  // ... until the % drops this far below the charge end

  Point points[MAX_POINTS];
  int pointCount;
  LogStats st;
  Point prev;      // last row read, carried across the files
  uint16_t prevC;  // drop reference % in 0.01 % (the previous row, or the last fractional one)
  bool prevFine;   // that % had a fraction
  bool prevUsb;
  uint16_t prevRowC;  // the previous row's % in 0.01 %, its precision, Wi-Fi and light
  bool prevRowFine;
  bool prevWifi;
  uint8_t prevLight;
  uint32_t stateChangeEpoch;  // last light, Wi-Fi, wake or boot change
  // The charge-end % while it may still be on a charger, 0 = not: with no VBUS
  // pin a charge that terminates on a wall adapter logs as unplugged, then sits
  // at 100%; those steps stay out of the drain sums.
  uint16_t fullHoldC;
  // Set before the first row (BatteryLogSum::load drops a battery.sum made with another value).
  uint16_t stateSkipS = 120;

  static void closeRun(LogStats& s) {
    if (s.run >= 0 && s.runS > 0) {  // run is 0, not -1, in a zeroed parser
      RunSums& r = s.netRuns[s.run];
      ++r.n;
      r.dt += static_cast<int64_t>(s.runC) * s.runS;
      r.d2 += static_cast<uint64_t>(static_cast<int64_t>(s.runC) * s.runC);
      r.t2 += static_cast<uint64_t>(s.runS) * s.runS;
    }
    s.run = -1;
    s.runC = 0;
    s.runS = 0;
  }

  static void endStretch(LogStats& s) {
    closeRun(s);
    for (int k = 0; k < 2; ++k) {
      if (s.netC[k] > 0) {
        s.dropC[k] += static_cast<uint32_t>(s.netC[k]);
        RunSums& r = s.runs[k];
        r.n += s.netRuns[k].n;
        r.dt += s.netRuns[k].dt;
        r.d2 += s.netRuns[k].d2;
        r.t2 += s.netRuns[k].t2;
      }
      s.netC[k] = 0;
      s.netRuns[k] = {};
    }
  }

  // The drop's ± squared in 0.01 %², category k. Runs drain d = r t + e with
  // the gauge's wander e independent per run (inside a run it cancels), so
  // var(drop) = n var(e), and var(e) is the runs' scatter around the fitted
  // rate: (sum d² - (sum d t)² / sum t²) / (n - 1), the 0.01 % rounding
  // included. RUN_ERR_C per run under 3 runs.
  static float errSq(const LogStats& s, const int k) {
    const RunSums& r = s.runs[k];
    if (r.n < 3 || r.t2 == 0) return static_cast<float>(r.n) * RUN_ERR_C;
    const double rss = static_cast<double>(r.d2) - static_cast<double>(r.dt) * r.dt / static_cast<double>(r.t2);
    return static_cast<float>(std::max(rss, 0.0) * r.n / (r.n - 1));
  }

  // "71.43" -> 7143, "71" -> 7100; fine = the field had a fraction.
  static uint16_t parseCenti(const char* f, bool& fine) {
    uint32_t v = 0;
    for (; *f >= '0' && *f <= '9'; ++f) v = std::min<uint32_t>(v * 10 + (*f - '0'), 100);
    v *= 100;
    fine = *f == '.';
    if (fine && f[1] >= '0' && f[1] <= '9') {
      v += (f[1] - '0') * 10;
      if (f[2] >= '0' && f[2] <= '9') v += f[2] - '0';
    }
    return static_cast<uint16_t>(std::min<uint32_t>(v, 10000));
  }

  // One CSV row (no newline). Header rows and rows without an RTC time are skipped.
  void parseRow(const char* line) {
    // Split once: f[10] (detail) is the rest of the row, commas and all.
    const char* f[11];
    int n = 1;
    f[0] = line;
    for (const char* p = line; *p && n < 11; ++p) {
      if (*p == ',') f[n++] = p + 1;
    }
    const uint32_t epoch = strtoul(line, nullptr, 10);
    if (epoch == 0 || n < 10) return;  // header, or no RTC time
    const char* detail = n > 10 ? f[10] : nullptr;
    enum Ev : uint8_t { OTHER, STATS_RESET, BOOT, WAKE, SLEEP, WIFI_ON, WIFI_OFF, CHARGED };
    Ev ev = OTHER;
    {
      const size_t len = detail ? static_cast<size_t>(detail - 1 - f[9]) : strlen(f[9]);
      static constexpr const char* NAMES[] = {"",      "stats_reset", "boot",     "wake",
                                              "sleep", "wifi_on",     "wifi_off", "charged"};
      for (int i = 1; i < 8; ++i) {
        if (strlen(NAMES[i]) == len && memcmp(f[9], NAMES[i], len) == 0) ev = static_cast<Ev>(i);
      }
    }
    // A blank % (logged before the gauge's first read) is the previous row's.
    const bool blank = *f[3] == ',';
    bool fine = prevRowFine;
    const uint16_t pctC = blank ? prevRowC : parseCenti(f[3], fine);
    const uint8_t pct = static_cast<uint8_t>(pctC / 100);
    // "charged" counts as plugged until the next row: the charger may have
    // stopped without a cable change (no VBUS pin, see fullHoldC).
    const bool usb = *f[6] == '1' || ev == CHARGED;
    const bool asleep = ev == SLEEP || (detail && strncmp(detail, "asleep", 6) == 0);
    const bool cold = ev == BOOT && detail && strstr(detail, "reset=POWERON");

    if (ev == STATS_RESET) {
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
      const bool onBattery = !prevUsb && !usb && fullHoldC == 0;
      if (prev.awake && onBattery && fine && prevRowFine &&
          (st.chargedEpoch == 0 || prev.epoch >= st.chargedEpoch + UNPLUG_SKIP_S) &&
          prev.epoch >= stateChangeEpoch + stateSkipS) {
        const int k = (prevWifi ? 2 : 0) + (prevLight ? 1 : 0);
        st.stateDropC[k] += static_cast<int32_t>(prevRowC) - pctC;
        st.stateS[k] += dt;
        st.stateDuty[k] += static_cast<uint64_t>(BatteryEstimate::lightDuty(prevLight)) * dt;
      }
      // Whole-percent steps (older rows, charger events logged asleep) are left out: each
      // adds +-1% to a drop of ~0.01%, which swamps the rate (the "0.03 +- 1.54%" asleep drain).
      if (onBattery && fine && prevFine) {
        st.battS[cat] += dt;
        st.netC[cat] += drop;
        if (st.run != cat) closeRun(st);
        st.run = static_cast<int8_t>(cat);
        st.runC += drop;
        st.runS += dt;
      } else if (onBattery && prevFine) {
        st.battS[cat] += dt;
      } else {
        endStretch(st);
      }
    } else {
      endStretch(st);
    }
    if (st.first == 0) st.first = epoch;
    st.last = epoch;
    if (ev == BOOT) ++(cold ? st.coldBoots : st.restarts);
    if (ev == WAKE) ++st.wakes;
    if (const char* w = detail ? strstr(detail, "false_wakes=") : nullptr) {
      st.falseWakes += strtoul(w + 12, nullptr, 10);
    }
    if (usb || *f[5] == '1') {
      if (!st.charging && (st.chargedEpoch == 0 || epoch - st.chargedEpoch >= CHARGE_MERGE_S)) {
        st.chargeFromC = pctC;
        st.chargeStartEpoch = epoch;
        st.chargeFromFine = fine;
      }
      st.charging = true;
    } else if (st.charging) {
      st.charging = false;
      st.chargedEpoch = epoch;
      fullHoldC = pctC >= FULL_C ? pctC : 0;
    }
    if (fullHoldC != 0 && fine && pctC + FULL_DROP_C <= fullHoldC) fullHoldC = 0;
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
    if (fine || !prevFine || usb || prevUsb || ev == STATS_RESET || cold) {
      prevC = pctC;
      prevFine = fine;
    }
    prevUsb = usb;
    prevRowC = pctC;
    prevRowFine = fine;
    if (ev == BOOT || ev == WAKE || ev == SLEEP || ev == WIFI_OFF) prevWifi = false;
    if (ev == WIFI_ON) prevWifi = true;
    const auto light = static_cast<uint8_t>(atoi(f[8]));
    if (ev == BOOT || ev == WAKE || ev == WIFI_ON || ev == WIFI_OFF || light != prevLight) stateChangeEpoch = epoch;
    prevLight = light;
    if (!blank) points[pointCount++] = prev;
  }
};
