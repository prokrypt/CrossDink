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
    // On battery, over the whole log; [0] awake, [1] asleep. In 0.01 %: drop,
    // the part of it from whole-percent rows, and its ± (each unbroken run of
    // steps adds its rows' precision, 1 or 100: inside a run the roundings cancel).
    uint32_t battS[2], dropC[2], coarseC[2], errC[2];
    int8_t run;  // category of the run the last step extended, -1 = none
    bool runFine;
    // The open stretch of on-battery steps: net drop per category, signed, so
    // the gauge's rise after an unplug cancels drops instead of being ignored.
    // Added to dropC (a negative net as 0) when the stretch ends.
    int32_t netC[2], netCoarseC[2];
    // Awake on battery, between fractional rows, by state [Wi-Fi * 2 + light on]:
    // signed drop in 0.01 %, seconds, and LED duty (BatteryEstimate::lightDuty)
    // x seconds. The first UNPLUG_SKIP_S after a charge is left out (the gauge
    // rises then).
    int32_t stateDropC[4];
    uint32_t stateS[4];
    uint64_t stateDuty[4];
  };
  static constexpr uint32_t CHARGE_MERGE_S = 60;
  static constexpr uint32_t UNPLUG_SKIP_S = 1800;
  static constexpr int MAX_POINTS = 400;

  Point points[MAX_POINTS];
  int pointCount;
  LogStats st;
  Point prev;             // last row read, carried across the files
  uint16_t prevC;         // drop reference % in 0.01 % (the previous row, or the last fractional one)
  bool prevFine;          // that % had a fraction
  bool prevUsb;
  uint16_t prevRowC;      // the previous row's % in 0.01 %, its precision, Wi-Fi and light
  bool prevRowFine;
  bool prevWifi;
  uint8_t prevLight;

  static void endStretch(LogStats& s) {
    for (int k = 0; k < 2; ++k) {
      if (s.netC[k] > 0) {
        s.dropC[k] += static_cast<uint32_t>(s.netC[k]);
        s.coarseC[k] += static_cast<uint32_t>(std::clamp<int32_t>(s.netCoarseC[k], 0, s.netC[k]));
      }
      s.netC[k] = s.netCoarseC[k] = 0;
    }
    s.run = -1;
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
    enum Ev : uint8_t { OTHER, STATS_RESET, BOOT, WAKE, SLEEP, WIFI_ON, WIFI_OFF };
    Ev ev = OTHER;
    {
      const size_t len = detail ? static_cast<size_t>(detail - 1 - f[9]) : strlen(f[9]);
      static constexpr const char* NAMES[] = {"", "stats_reset", "boot", "wake", "sleep", "wifi_on", "wifi_off"};
      for (int i = 1; i < 7; ++i) {
        if (strlen(NAMES[i]) == len && memcmp(f[9], NAMES[i], len) == 0) ev = static_cast<Ev>(i);
      }
    }
    bool fine = false;
    const uint16_t pctC = parseCenti(f[3], fine);
    const uint8_t pct = static_cast<uint8_t>(pctC / 100);
    const bool usb = *f[6] == '1';
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
      if (prev.awake && !prevUsb && !usb && fine && prevRowFine &&
          (st.chargedEpoch == 0 || prev.epoch >= st.chargedEpoch + UNPLUG_SKIP_S)) {
        const int k = (prevWifi ? 2 : 0) + (prevLight ? 1 : 0);
        st.stateDropC[k] += static_cast<int32_t>(prevRowC) - pctC;
        st.stateS[k] += dt;
        st.stateDuty[k] += static_cast<uint64_t>(BatteryEstimate::lightDuty(prevLight)) * dt;
      }
      if (!prevUsb && !usb && fine == prevFine) {
        st.battS[cat] += dt;
        st.netC[cat] += drop;
        if (!fine) st.netCoarseC[cat] += drop;
        if (st.run != cat || st.runFine != fine) st.errC[cat] += fine ? 1 : 100;
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
    if (fine || !prevFine || usb || prevUsb || ev == STATS_RESET || cold) {
      prevC = pctC;
      prevFine = fine;
    }
    prevUsb = usb;
    prevRowC = pctC;
    prevRowFine = fine;
    if (ev == BOOT || ev == WAKE || ev == SLEEP || ev == WIFI_OFF) prevWifi = false;
    if (ev == WIFI_ON) prevWifi = true;
    prevLight = static_cast<uint8_t>(atoi(f[8]));
    points[pointCount++] = prev;
  }
};
