#pragma once

// One /debug/logs/power.csv row (src/util/PowerLog). Arduino-free so host tests
// can check it against the header; scripts/power_fit.py reads these columns
// by name.

#include <PowerCounters.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace PowerLogRow {

// Cumulative columns from awake_ms on (gen keys them: never subtract across a
// change of gen). *_ms are milliseconds, light_full_ms is ms at full LED duty,
// ref_* are refresh counts from HalDisplay::refreshCounts(), panel_*_ms the
// time spent waiting on the panel's BUSY line after that kind of refresh.
constexpr char kHeader[] =
    "epoch_utc,local_time,uptime_ms,gen,event,detail,pct,mv,temp_c,panel_c,chg,usb,chg_seen,wifi,"
    "awake_ms,asleep_s,asleep_cw_s,ls_ms,maxclk_ms,busy0_ms,busy1_ms,"
    "wifi_up_ms,wifi_ps_ms,wifi_awake_ms,wifi_ap_ms,scans,connects,ip_tx,ip_rx,"
    "light_full_ms,ref_full,ref_half,ref_fast,ref_gray,ref_flash,"
    "panel_full_ms,panel_half_ms,panel_fast_ms,panel_gray_ms,booster_ms,sd_rd_kb,sd_wr_kb,sd_ms\n";

// Fits a row with every field at its type's maximum (PowerLogRow tests check
// this), so format() never runs out of room in a buffer this size.
constexpr size_t kMaxRow = 768;

inline const char* wifiName(const PowerCounters::RadioState s) {
  static const char* const kNames[] = {"off", "up", "ps", "awake", "ap"};
  return s < PowerCounters::RADIO_STATES ? kNames[s] : "?";
}

struct Row {
  uint32_t epoch = 0;  // 0 = clock not set: epoch and local_time are left empty
  int32_t localOffsetS = 0;
  uint32_t uptimeMs = 0;
  uint16_t gen = 0;
  const char* event = "";
  const char* detail = nullptr;
  uint16_t pct = 0;
  uint16_t pct256 = 0;  // 0 = whole percent only
  uint16_t mv = 0;
  int16_t tempDeciC = 0;
  bool tempKnown = false;
  int8_t panelC = 0;
  bool panelKnown = false;
  bool chg = false;
  bool usb = false;
  bool chargerSeen = false;
  PowerCounters::RadioState wifi = PowerCounters::RADIO_OFF;
  PowerCounters::Totals t{};
  uint32_t refresh[5] = {};  // HalDisplay::refreshCounts().n: full, half, fast, gray, flashing
};

// Writes the row with its newline; returns its length, or 0 if it did not fit.
// Commas in detail become ';' so the column count holds.
inline size_t format(char* out, const size_t size, const Row& r) {
  using ull = unsigned long long;
  char local[20] = "";
  char epoch[12] = "";
  if (r.epoch != 0) {
    snprintf(epoch, sizeof(epoch), "%lu", static_cast<unsigned long>(r.epoch));
    const time_t t = static_cast<time_t>(r.epoch) + r.localOffsetS;
    tm parts{};
    gmtime_r(&t, &parts);
    strftime(local, sizeof(local), "%Y-%m-%d %H:%M:%S", &parts);
  }
  char detail[48] = "";
  if (r.detail) {
    size_t i = 0;
    for (; r.detail[i] && i + 1 < sizeof(detail); ++i)
      detail[i] = r.detail[i] == ',' || r.detail[i] == '\n' ? ';' : r.detail[i];
    detail[i] = '\0';
  }
  char pct[8] = "";
  if (r.pct256 != 0) {
    snprintf(pct, sizeof(pct), "%u.%02u", static_cast<unsigned>(r.pct256 >> 8),
             static_cast<unsigned>((r.pct256 & 0xFF) * 100 / 256));
  } else if (r.pct != 0) {
    snprintf(pct, sizeof(pct), "%u", static_cast<unsigned>(r.pct));
  }
  char temp[8] = "";
  if (r.tempKnown) {
    const int d = r.tempDeciC < 0 ? -r.tempDeciC : r.tempDeciC;
    snprintf(temp, sizeof(temp), "%s%d.%d", r.tempDeciC < 0 ? "-" : "", d / 10, d % 10);
  }
  char panel[6] = "";
  if (r.panelKnown) snprintf(panel, sizeof(panel), "%d", r.panelC);
  const PowerCounters::Totals& t = r.t;
  const int n = snprintf(
      out, size,
      "%s,%s,%lu,%u,%s,%s,%s,%u,%s,%s,%u,%u,%u,%s,"
      "%llu,%lu,%lu,%llu,%llu,%llu,%llu,"
      "%llu,%llu,%llu,%llu,%lu,%lu,%llu,%llu,"
      "%llu,%lu,%lu,%lu,%lu,%lu,"
      "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
      epoch, local, static_cast<unsigned long>(r.uptimeMs), static_cast<unsigned>(r.gen), r.event, detail, pct,
      static_cast<unsigned>(r.mv), temp, panel, r.chg ? 1u : 0u, r.usb ? 1u : 0u, r.chargerSeen ? 1u : 0u,
      wifiName(r.wifi), static_cast<ull>(t.awakeMs), static_cast<unsigned long>(t.asleepS[0]),
      static_cast<unsigned long>(t.asleepS[1]), static_cast<ull>(t.lightSleepUs / 1000),
      static_cast<ull>(t.maxClockUs / 1000), static_cast<ull>(t.busyUs[0] / 1000), static_cast<ull>(t.busyUs[1] / 1000),
      static_cast<ull>(t.wifiMs[PowerCounters::RADIO_UP]), static_cast<ull>(t.wifiMs[PowerCounters::RADIO_PS]),
      static_cast<ull>(t.wifiMs[PowerCounters::RADIO_AWAKE]), static_cast<ull>(t.wifiMs[PowerCounters::RADIO_AP]),
      static_cast<unsigned long>(t.wifiScans), static_cast<unsigned long>(t.wifiConnects),
      static_cast<ull>(t.ipTxPackets), static_cast<ull>(t.ipRxPackets), static_cast<ull>(t.lightDutyMs / 1023),
      static_cast<unsigned long>(r.refresh[0]), static_cast<unsigned long>(r.refresh[1]),
      static_cast<unsigned long>(r.refresh[2]), static_cast<unsigned long>(r.refresh[3]),
      static_cast<unsigned long>(r.refresh[4]), static_cast<ull>(t.panelBusyMs[PowerCounters::PANEL_FULL]),
      static_cast<ull>(t.panelBusyMs[PowerCounters::PANEL_HALF]),
      static_cast<ull>(t.panelBusyMs[PowerCounters::PANEL_FAST]),
      static_cast<ull>(t.panelBusyMs[PowerCounters::PANEL_GRAY]), static_cast<ull>(t.boosterMs),
      static_cast<ull>(t.sdReadBytes / 1024), static_cast<ull>(t.sdWriteBytes / 1024), static_cast<ull>(t.sdUs / 1000));
  if (n <= 0 || static_cast<size_t>(n) >= size) return 0;
  return static_cast<size_t>(n);
}

}  // namespace PowerLogRow
