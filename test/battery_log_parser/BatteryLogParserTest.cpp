#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "src/util/BatteryLogParser.h"

namespace {

const char* const ROWS[] = {
    "epoch_utc,local_time,uptime_ms,pct,mv,chg,usb,temp_c,light_pct,event,detail",
    "1790896747,2026-10-01 16:19:07,549,82.30,4020,0,0,32.5,0,boot,reset=POWERON wake=UNDEFINED",
    "1790896760,2026-10-01 16:19:20,13000,82.28,4019,0,0,32.5,0,wifi_on,",
    "1790897000,2026-10-01 16:23:20,253000,82.10,4015,0,0,32.5,30,light,",
    "1790897600,2026-10-01 16:33:20,853000,81.70,4010,0,0,32.5,30,pct,",
    "1790897700,2026-10-01 16:35:00,953000,81.65,4009,0,0,32.5,0,wifi_off,",
    "1790897800,2026-10-01 16:36:40,1053000,81.60,4008,0,0,32.5,0,sleep,request",
    "1790900000,2026-10-01 17:13:20,0,81,4100,1,1,,0,chg_on,asleep",
    "1790901000,2026-10-01 17:30:00,712,85.00,4180,1,1,31.0,0,wake,reset=DEEPSLEEP wake=EXT1 false_wakes=2 "
    "awake_ms=248",
    "1790904000,2026-10-01 18:20:00,3000712,95.10,4190,0,1,31.0,0,charged,",
    "1790904100,2026-10-01 18:21:40,3100712,95.10,4190,0,0,31.0,0,usb_out,",
    "1790910000,2026-10-01 20:00:00,9000712,94.00,4170,0,0,30.0,10,light,",
    "1790911000,2026-10-01 20:16:40,10000712,93.50,4160,0,0,30.0,10,pct,",
    "1790911100,2026-10-01 20:18:20,10100712,93.40,4160,0,0,30.0,10,stats_reset,",
    "1790912000,2026-10-01 20:33:20,11000712,93.00,4150,0,0,30.0,10,pct,",
    "1790913000,2026-10-01 20:50:00,12000712,92.50,4140,0,0,30.0,0,boot,reset=SW wake=UNDEFINED",
    "0,1970-01-01 00:00:00,5,92,4140,0,0,30.0,0,boot,no clock",
    "1790914000,2026-10-01 21:06:40,1000,92.00,4130,0,0,30.0,0,sleep,idle-timeout",
};
constexpr int N = sizeof(ROWS) / sizeof(ROWS[0]);

// Every field, so padding bytes don't count.
std::string dump(const BatteryLogParser& p) {
  const auto& s = p.st;
  char b[512];
  std::string o;
  snprintf(b, sizeof(b), "%u %u %d %u %u %u %u %u %u %u %u %u %u %d %d %d %d|", s.first, s.last, s.reset, s.coldBoots,
           s.restarts, s.wakes, s.falseWakes, s.awakeS, s.asleepS, s.chargedEpoch, s.chargeStartEpoch, s.chargeFromC,
           s.chargeToC, s.chargeFromFine, s.chargeToFine, s.charging, s.run);
  o += b;
  snprintf(b, sizeof(b), "%d %u|", s.runC, s.runS);
  o += b;
  for (int k = 0; k < 2; ++k) {
    for (const auto& r : {s.runs[k], s.netRuns[k]}) {
      snprintf(b, sizeof(b), "%u %lld %llu %llu ", r.n, static_cast<long long>(r.dt),
               static_cast<unsigned long long>(r.d2), static_cast<unsigned long long>(r.t2));
      o += b;
    }
    snprintf(b, sizeof(b), "%u %u %d|", s.battS[k], s.dropC[k], s.netC[k]);
    o += b;
  }
  for (int k = 0; k < 4; ++k) {
    snprintf(b, sizeof(b), "%d %u %llu|", s.stateDropC[k], s.stateS[k],
             static_cast<unsigned long long>(s.stateDuty[k]));
    o += b;
  }
  snprintf(b, sizeof(b), "%d %u %d %d %u %u %d %d %d %d %d %u %u %u|", p.pointCount, p.prevC, p.prevFine, p.prevUsb,
           p.prevRowC, p.prev.epoch, p.prev.pct, p.prev.awake, p.prevRowFine, p.prevWifi, p.prevLight,
           p.stateChangeEpoch, p.fullHoldC, p.stateSkipS);
  o += b;
  for (int i = 0; i < p.pointCount; ++i) {
    snprintf(b, sizeof(b), "%u,%u,%d;", p.points[i].epoch, p.points[i].pct, p.points[i].awake);
    o += b;
  }
  return o;
}

}  // namespace

// battery.sum is the raw parser bytes after a row boundary: resuming from them
// and reading the rest must give what one pass over the whole log gives.
TEST(BatteryLogParser, ResumeFromSavedBytesMatchesOnePass) {
  static BatteryLogParser full{};
  for (const char* row : ROWS) full.parseRow(row);
  for (int cut = 0; cut <= N; ++cut) {
    static BatteryLogParser head{}, resumed;
    head = {};
    for (int i = 0; i < cut; ++i) head.parseRow(ROWS[i]);
    memcpy(static_cast<void*>(&resumed), &head, sizeof(head));  // save + load
    for (int i = cut; i < N; ++i) resumed.parseRow(ROWS[i]);
    EXPECT_EQ(dump(resumed), dump(full)) << "cut at row " << cut;
  }
  EXPECT_TRUE(full.st.reset);
  EXPECT_EQ(full.st.restarts, 1u);    // after the stats_reset row
  EXPECT_EQ(full.pointCount, N - 2);  // header and the no-clock row are skipped
}

TEST(BatteryLogParser, PercentParse) {
  bool fine = false;
  EXPECT_EQ(BatteryLogParser::parseCenti("71.43,", fine), 7143);
  EXPECT_TRUE(fine);
  EXPECT_EQ(BatteryLogParser::parseCenti("71,", fine), 7100);
  EXPECT_FALSE(fine);
  EXPECT_EQ(BatteryLogParser::parseCenti("100.00,", fine), 10000);
  EXPECT_EQ(BatteryLogParser::parseCenti("0.05,", fine), 5);
}

// Whole-percent steps must not add error or drop: two whole rows 1% apart used to add +-1%.
TEST(BatteryLogParser, WholePercentStepsAreSkipped) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,80,4000,0,0,30,0,sleep,", "1790899600,x,0,79,3990,0,0,30,0,wake,",
                          "1790903200,x,0,79.00,3990,0,0,30,0,pct,", "1790906800,x,0,78.99,3989,0,0,30,0,pct,"}) {
    p.parseRow(row);
  }
  EXPECT_EQ(p.st.runS, 3600u);                // only the fine->fine step, still open
  EXPECT_EQ(p.st.netC[0] + p.st.netC[1], 1);  // open stretch, not yet in dropC
  BatteryLogParser::endStretch(p.st);
  EXPECT_EQ(p.st.runs[0].n + p.st.runs[1].n, 1u);
  EXPECT_FLOAT_EQ(BatteryLogParser::errSq(p.st, 0) + BatteryLogParser::errSq(p.st, 1), BatteryLogParser::RUN_ERR_C);
}

// 1002z on .67: 50 short sleeps at a steady 0.75 %/day read "0.75 ±5.71%/day"
// when every run added 0.25 %. The ± now comes from the runs' own scatter.
TEST(BatteryLogParser, ErrorFollowsRunScatter) {
  static BatteryLogParser p{};
  char row[96];
  uint32_t t = 1790896000;
  uint32_t c = 8000;
  for (int i = 0; i < 50; ++i) {
    snprintf(row, sizeof(row), "%u,x,0,%u.%02u,4000,0,0,30,0,sleep,", t, c / 100, c % 100);
    p.parseRow(row);
    t += 540;
    c -= i % 2;  // 0.75 %/day is 0.005 % per 9 min: the gauge shows 0.00 or 0.01
    snprintf(row, sizeof(row), "%u,x,0,%u.%02u,4000,0,0,30,0,wake,", t, c / 100, c % 100);
    p.parseRow(row);
    t += 60;
    c -= 2;
  }
  snprintf(row, sizeof(row), "%u,x,0,%u.%02u,4000,0,0,30,0,sleep,", t, c / 100, c % 100);
  p.parseRow(row);
  BatteryLogParser::endStretch(p.st);
  EXPECT_EQ(p.st.runs[1].n, 50u);
  EXPECT_EQ(p.st.dropC[1], 25u);
  // Old model: 50 x (0.25 %)² = (1.77 %)² on a 0.25 % drop. Fitted: the 0.00/0.01
  // scatter, (0.036 %)².
  EXPECT_NEAR(BatteryLogParser::errSq(p.st, 1), 12.5f * 50 / 49, 0.01f);
}

// Awake runs scatter with use (reading, Wi-Fi, transfers): the 0.25 % per run bound wins.
TEST(BatteryLogParser, ScatterAboveModelKeepsModel) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,80.00,4000,0,0,30,0,wake,", "1790899600,x,0,78.00,4000,0,0,30,0,sleep,",
                          "1790899700,x,0,78.00,4000,0,0,30,0,wake,", "1790903300,x,0,77.90,4000,0,0,30,0,sleep,",
                          "1790903400,x,0,77.90,4000,0,0,30,0,wake,", "1790907000,x,0,75.90,4000,0,0,30,0,sleep,",
                          "1790907100,x,0,75.90,4000,0,0,30,0,wake,", "1790910700,x,0,75.80,4000,0,0,30,0,sleep,"}) {
    p.parseRow(row);
  }
  BatteryLogParser::endStretch(p.st);
  EXPECT_EQ(p.st.runs[0].n, 4u);
  EXPECT_FLOAT_EQ(BatteryLogParser::errSq(p.st, 0), 4.0f * BatteryLogParser::RUN_ERR_C);
}

// A stretch that netted a rise adds neither drop nor ±.
TEST(BatteryLogParser, RiseAddsNoError) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,80.00,4000,0,0,30,0,pct,", "1790899600,x,0,80.10,4010,0,0,30,0,pct,"}) {
    p.parseRow(row);
  }
  BatteryLogParser::endStretch(p.st);
  EXPECT_EQ(p.st.dropC[0], 0u);
  EXPECT_EQ(p.st.runs[0].n, 0u);
}

// Wall adapter: charging stops at 100% and logs as unplugged; the flat hours on
// the charger stay out of the drain until the gauge drops 0.05%.
TEST(BatteryLogParser, FullChargeEndHoldsUntilDrop) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,99.00,4190,1,1,30,0,chg_on,", "1790897000,x,0,100.00,4200,0,0,30,0,chg_off,",
                          "1790917000,x,0,100.00,4200,0,0,30,0,pct,", "1790918000,x,0,99.96,4195,0,0,30,0,pct,",
                          "1790919000,x,0,99.94,4194,0,0,30,0,pct,", "1790922600,x,0,99.80,4190,0,0,30,0,pct,"}) {
    p.parseRow(row);
  }
  BatteryLogParser::endStretch(p.st);
  EXPECT_EQ(p.st.battS[0], 3600u);  // only from 99.94 on
  EXPECT_EQ(p.st.dropC[0], 14u);
}

// "charged" with the cable reading out: the step after it is still plugged.
TEST(BatteryLogParser, ChargedRowCountsAsPlugged) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,90.00,4190,1,1,30,0,chg_on,", "1790897000,x,0,96.00,4200,0,0,30,0,charged,",
                          "1790900600,x,0,95.90,4190,0,0,30,0,usb_out,"}) {
    p.parseRow(row);
  }
  EXPECT_EQ(p.st.battS[0], 0u);
  EXPECT_EQ(p.st.chargedEpoch, 1790900600u);
  EXPECT_EQ(p.st.chargeToC, 9590u);
}

// Per-state sums skip steps starting under stateSkipS after a light or Wi-Fi change.
TEST(BatteryLogParser, StateSumsSkipAfterChange) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,80.00,4000,0,0,30,0,pct,", "1790896600,x,0,79.90,4000,0,0,30,0,pct,",
                          "1790897200,x,0,79.80,4000,0,0,30,40,light,", "1790897350,x,0,79.60,4000,0,0,30,40,pct,",
                          "1790897950,x,0,79.50,4000,0,0,30,40,pct,"}) {
    p.parseRow(row);
  }
  EXPECT_EQ(p.st.stateS[0], 1200u);  // light off: both steps before the light row
  EXPECT_EQ(p.st.stateS[1], 600u);   // light on: the dip in the first 150 s is skipped
  EXPECT_EQ(p.st.stateDropC[1], 10);
  EXPECT_EQ(p.st.battS[0], 1950u);  // the overall drain keeps every step
}

// A row logged before the gauge's first read has a blank %: it reads as the
// previous row's, so it is no drop and no graph point.
TEST(BatteryLogParser, BlankPercentIsNoReading) {
  static BatteryLogParser p{};
  for (const char* row : {"1790896000,x,0,80.00,4000,0,0,30,0,pct,", "1790896600,x,0,,4000,0,0,30,0,boot,reset=SW",
                          "1790897200,x,0,79.80,4000,0,0,30,0,pct,"}) {
    p.parseRow(row);
  }
  BatteryLogParser::endStretch(p.st);
  EXPECT_EQ(p.pointCount, 2);
  EXPECT_EQ(p.st.restarts, 1u);
  EXPECT_EQ(p.st.dropC[0], 20u);
  EXPECT_EQ(p.st.battS[0], 1200u);
}
