#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "src/util/PowerLogRow.h"

namespace {

size_t columns(const char* text) { return static_cast<size_t>(std::count(text, text + strlen(text), ',')) + 1; }

PowerLogRow::Row sample() {
  PowerLogRow::Row r;
  r.epoch = 1790000000;  // 2026-09-21 14:13:20 UTC
  r.localOffsetS = -7 * 3600;
  r.uptimeMs = 123456;
  r.gen = 42;
  r.event = "test_start";
  r.detail = "light100";
  r.pct = 71;
  r.pct256 = 71 * 256 + 110;  // 71.42 %
  r.mv = 3950;
  r.tempDeciC = -15;
  r.tempKnown = true;
  r.panelC = 21;
  r.panelKnown = true;
  r.wifi = PowerCounters::RADIO_PS;
  r.t.awakeMs = 3600000;
  r.t.asleepS[0] = 100;
  r.t.asleepS[1] = 200;
  r.t.lightSleepUs = 1500000;
  r.t.lightDutyMs = 1023ull * 60000;  // one minute at full duty
  r.t.sdReadBytes = 2048;
  r.refresh[2] = 17;
  return r;
}

}  // namespace

TEST(PowerLogRow, MatchesTheHeader) {
  char out[448];
  const size_t n = PowerLogRow::format(out, sizeof(out), sample());
  ASSERT_GT(n, 0u);
  EXPECT_EQ(out[n - 1], '\n');
  EXPECT_EQ(columns(out), columns(PowerLogRow::kHeader));
}

TEST(PowerLogRow, Values) {
  char out[448];
  ASSERT_GT(PowerLogRow::format(out, sizeof(out), sample()), 0u);
  const std::string row(out);
  EXPECT_EQ(row.rfind("1790000000,2026-09-21 07:13:20,123456,42,test_start,light100,71.42,3950,-1.5,21,0,0,0,ps,"
                      "3600000,100,200,1500,",
                      0),
            0u)
      << row;
  EXPECT_NE(row.find(",60000,0,0,17,0,0,"), std::string::npos) << row;  // light_full_ms, refresh counts
}

TEST(PowerLogRow, UnknownsStayEmpty) {
  PowerLogRow::Row r;
  r.event = "boot";
  char out[448];
  ASSERT_GT(PowerLogRow::format(out, sizeof(out), r), 0u);
  EXPECT_EQ(std::string(out).rfind(",,0,0,boot,,,0,,,0,0,0,off,", 0), 0u) << out;
}

TEST(PowerLogRow, DetailCommasCannotAddColumns) {
  PowerLogRow::Row r = sample();
  r.detail = "a,b,c";
  char out[448];
  ASSERT_GT(PowerLogRow::format(out, sizeof(out), r), 0u);
  EXPECT_EQ(columns(out), columns(PowerLogRow::kHeader));
  EXPECT_NE(std::string(out).find(",a;b;c,"), std::string::npos);
}

TEST(PowerLogRow, EveryFieldAtItsMaximumFitsKMaxRow) {
  PowerLogRow::Row r;
  r.epoch = std::numeric_limits<uint32_t>::max();
  r.uptimeMs = std::numeric_limits<uint32_t>::max();
  r.gen = std::numeric_limits<uint16_t>::max();
  r.event = "test_abort";
  r.detail = "refresh dmv=-1234.5 sd=1234.5 n=255 base=65535 and then some more";
  r.pct256 = std::numeric_limits<uint16_t>::max();
  r.mv = std::numeric_limits<uint16_t>::max();
  r.tempDeciC = std::numeric_limits<int16_t>::min();
  r.tempKnown = true;
  r.panelC = std::numeric_limits<int8_t>::min();
  r.panelKnown = true;
  r.chg = r.usb = r.chargerSeen = true;
  r.wifi = PowerCounters::RADIO_AWAKE;
  constexpr uint64_t u64 = std::numeric_limits<uint64_t>::max();
  constexpr uint32_t u32 = std::numeric_limits<uint32_t>::max();
  PowerCounters::Totals& t = r.t;
  t.awakeMs = t.lightSleepUs = t.maxClockUs = t.ipTxPackets = t.ipRxPackets = t.lightDutyMs = t.boosterMs = u64;
  t.sdReadBytes = t.sdWriteBytes = t.sdUs = u64;
  t.asleepS[0] = t.asleepS[1] = t.wifiScans = t.wifiConnects = u32;
  for (auto& v : t.busyUs) v = u64;
  for (auto& v : t.wifiMs) v = u64;
  for (auto& v : t.panelBusyMs) v = u64;
  for (auto& v : r.refresh) v = u32;
  std::string out(PowerLogRow::kMaxRow, '\0');
  const size_t n = PowerLogRow::format(out.data(), out.size(), r);
  EXPECT_GT(n, 0u);
  EXPECT_EQ(columns(out.c_str()), columns(PowerLogRow::kHeader));
}

TEST(PowerLogRow, TooSmallBufferWritesNothingUsable) {
  // A size the compiler cannot see keeps -Wformat-truncation out of a deliberate overflow.
  volatile size_t size = 32;
  std::string out(size, '\0');
  EXPECT_EQ(PowerLogRow::format(out.data(), size, sample()), 0u);
}
