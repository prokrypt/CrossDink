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
    "1790901000,2026-10-01 17:30:00,712,85.00,4180,1,1,31.0,0,wake,reset=DEEPSLEEP wake=EXT1 false_wakes=2 awake_ms=248",
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
  snprintf(b, sizeof(b), "%u %u %d %u %u %u %u %u %u %u %u %u %u %d %d %d %d %d|", s.first, s.last, s.reset,
           s.coldBoots, s.restarts, s.wakes, s.falseWakes, s.awakeS, s.asleepS, s.chargedEpoch, s.chargeStartEpoch,
           s.chargeFromC, s.chargeToC, s.chargeFromFine, s.chargeToFine, s.charging, s.run, s.runFine);
  o += b;
  for (int k = 0; k < 2; ++k) {
    snprintf(b, sizeof(b), "%u %u %u %u %d %d|", s.battS[k], s.dropC[k], s.coarseC[k], s.errC[k], s.netC[k],
             s.netCoarseC[k]);
    o += b;
  }
  for (int k = 0; k < 4; ++k) {
    snprintf(b, sizeof(b), "%d %u %llu|", s.stateDropC[k], s.stateS[k],
             static_cast<unsigned long long>(s.stateLight[k]));
    o += b;
  }
  snprintf(b, sizeof(b), "%d %u %d %d %u %u %d %d %d %d %d|", p.pointCount, p.prevC, p.prevFine, p.prevUsb, p.prevRowC,
           p.prev.epoch, p.prev.pct, p.prev.awake, p.prevRowFine, p.prevWifi, p.prevLight);
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
  EXPECT_EQ(full.st.restarts, 1u);  // after the stats_reset row
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
