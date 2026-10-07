# Power counters (debug builds)

The X4 Pro's CW2017 gauge reports state of charge (to 1/256 %), cell voltage and temperature, but no current.
To find out what each function costs, `x4-pro-debug` builds count the activity that drains the battery and
log it next to the gauge reading. `scripts/power_fit.py` then works out a cost per function from many intervals
of everyday use and from controlled runs (Goodies > Power Test).

## `/debug/logs/power.csv`

Written by `src/util/PowerLog.cpp` (driven by the battery log, `src/util/BatteryLog.cpp`), one row:

- at every boot or wake and before every deep sleep;
- when the whole battery % changes;
- when Wi-Fi turns on or off;
- every Goodies > Knobs `powerTickMin` minutes awake (default 10);
- for Power Test marks (`test_start`, `test_end`, `test_abort`, `sag`) and a stats reset (`reset`).

Rows go to a 16 KB PSRAM ring first and reach the card before deep sleep, soon after a restart or crash, or when
the ring is 3/4 full (with 2 s of no input). At 256 KB the file becomes `power.1.csv` (one old copy, about two
weeks of rows).

Every counter column is cumulative. Subtract two rows of the same `gen` to get the activity between them:

| Column | Meaning |
| --- | --- |
| `gen` | Counter generation. Changes after power loss, a crash or watchdog reset (counts since the last save are lost) or a Battery & stats reset. Never subtract across a change. |
| `pct`, `mv`, `temp_c` | Gauge reading (same as `battery.csv`); `pct` has 2 decimals from the gauge's fraction. |
| `panel_c` | UC8179 panel temperature, if sampled in the last 10 min (refresh waveforms get longer in the cold). |
| `chg`, `usb`, `chg_seen` | Charging or USB now, or seen since the previous row (including charging while asleep). |
| `wifi` | Radio state now: `off`, `up` (on, not associated), `ps` (modem sleep), `awake` (power save off), `ap`. |
| `awake_ms` | Time awake. |
| `asleep_s`, `asleep_cw_s` | Deep sleep with the charger-status wake off and on (see below). Includes the brief wakes that go straight back to sleep. |
| `ls_ms` | Automatic light sleep while awake (`CONFIG_PM_LIGHT_SLEEP_CALLBACKS`). |
| `maxclk_ms` | Time `HalPowerManager` holds the full-clock PM lock. The rest of awake time is at the 80 MHz DFS floor. |
| `busy0_ms`, `busy1_ms` | Per core, time outside the FreeRTOS idle task. |
| `wifi_up_ms` .. `wifi_ap_ms` | Time in each radio state (sampled once a second). |
| `scans`, `connects` | Wi-Fi scans finished and station connections made. |
| `ip_tx`, `ip_rx` | IP packets sent and received (lwIP, `CONFIG_LWIP_STATS`). |
| `light_full_ms` | Frontlight time weighted by PWM duty: ms at full duty. 50% brightness is about a third of full duty. Ducks, Light Timeout and transfer pulses included. |
| `ref_full` .. `ref_flash` | Refresh counts from `HalDisplay::refreshCounts()` (zeroed by a stats reset and by power loss). |
| `panel_*_ms` | Time spent waiting on the panel's BUSY line, by the kind of the last refresh started. An async refresh that finishes before anyone waits is not in it, so prefer the counts. |
| `booster_ms` | UC8179 booster on between refreshes (until `powerOffIdle()`); 0 on other panels. |
| `sd_rd_kb`, `sd_wr_kb`, `sd_ms` | Bytes and time through `HalFile::read/write` (not USB Drive). |

## Knobs

- `chargeWake` (Power, default 1): the charger status line wakes the device from deep sleep so the battery log
  can record charging. Arming that wake (ext0) keeps the RTC peripherals powered through sleep, which may cost
  sleep current. Set it to 0 for a few nights: sleep time then lands in `asleep_s` instead of `asleep_cw_s`, and
  the fit gives both costs. With 0, charging while asleep is no longer logged.
- `powerTickMin` (Power, default 10): minutes between periodic rows while awake.
- `powerTestMin` (Power, default 30): Power Test run length.
- `powerTestRefreshS` (Power, default 10): seconds between refreshes in the refresh loop tests.

## Goodies > Power Test

Each test holds one load for `powerTestMin` with everything else idle (Wi-Fi off and the frontlight off unless
the test turns them on; the Wi-Fi remote pauses), keeps the device out of auto sleep, and writes `test_start` and
`test_end` rows. Back stops it early (`test_abort`). The page shows the drop, the rate once the drop passes 0.3 %,
the cell voltage and what the counters saw. Confirm updates the page; it otherwise only redraws at the start and
end, so its own refreshes stay out of the run.

| Test | Load |
| --- | --- |
| Idle | nothing: the baseline the others are compared with |
| Light 50%, Light 100% | frontlight at that brightness (two levels check that cost follows duty) |
| Wi-Fi idle (modem sleep) | joined to the last saved network, modem sleep, CPU allowed to light-sleep (as idle File Transfer) |
| Wi-Fi awake | joined, power save off, full clock (as a Wi-Fi screen during a transfer) |
| CPU busy (one core) | a task spinning 9 ms of every 10 on the worker core at full clock |
| Fast refresh loop, Full refresh loop | half the screen black, swapping sides every `powerTestRefreshS` |
| Voltage sag probe | about 2 min: see below |

Run them with a charged battery off USB, at room temperature, and at least 30 min each: the gauge needs a drop of
a few tenths of a percent before a rate means much. Charging or USB during a run makes it useless (the script says
so).

### Voltage sag probe

The cell's voltage drops in step with the current drawn (by the current times the cell's internal resistance).
The probe switches each load (light 100%, CPU spin, Wi-Fi receiver on, one Full refresh) on and off three times,
averages the voltage for 2 s before and while it is on, and logs `sag` rows such as
`light dmv=-23.4 sd=1.2 n=3 base=3950`. A bigger drop means more current, so the loads can be ranked in two
minutes instead of hours. The page also shows the gauge's read noise. Internal resistance changes with charge and
temperature, so compare loads from the same run, not across days.

## Fitting

```sh
python3 scripts/power_fit.py power.csv power.1.csv --capacity-mah 2000
```

Copy the files off the card (or use the web portal's Logs page), and pass the cell's rated capacity to get mA
beside %/h (1 %/h = capacity / 100 mA). The script:

1. lists the Power Test runs with their drop and rate, and each run's rate less the Idle run's (that load's own
   cost);
2. cuts the log into back-to-back on-battery intervals of at least `--min-drop` % (default 1.0), never across a
   charge, USB, a `gen` change or a counter that went backwards;
3. fits the drop of every interval as a sum of activity x cost with all costs >= 0, and prints each cost with a
   90% range from resampling and its share of all the drain logged.

The costs add up: `floor_h` is the awake base at the clock floor, `maxclk_h` is the cost of an hour at full clock
instead (not on top of the floor), `ls_h` an hour light-sleeping, and `wifi_*_h`, `light_full_h` and the refresh
costs come on top of whichever clock state the CPU was in. `--all-features` adds core busy time, IP packets,
booster time and SD traffic; `--features a,b,c` picks columns. A cost of 0 with a wide range means the log never
varied it on its own: run its Power Test. The cross-validated error against "one average rate per hour" shows
whether the split explains anything.

## Caveats

- The gauge infers charge from voltage, so a heavy load reads low for a while and recovers. Short intervals
  mis-assign drain; the 1 % minimum and many intervals average this out.
- Debug builds log more than release builds (PSRAM log, `[SYS]`/`[PM]` lines, the battery and power logs), so
  their awake base is a little higher than a release build's.
- Light sleep, packets and the `[PM]` profiling need the `x4-pro-debug` sdkconfig
  (`CONFIG_PM_LIGHT_SLEEP_CALLBACKS`, `CONFIG_LWIP_STATS`). Other builds compile all of this out.

## Checking it on a device

1. Flash `x4-pro-debug`, use the device for a day, then look for `/debug/logs/power.csv` (it is written at the
   first sleep). Serial and the PSRAM log show each row as a `[PWL]` line, and a `Power counters gen N` line at boot.
2. Run Goodies > Power Test > Light 100% for 30 min off USB: the page should show `At full light 30m`, and
   `power.csv` a `test_start` / `test_end` pair 30 min apart with `light_full_ms` up by about 1,800,000.
3. Run the voltage sag probe: four `sag` rows; the light and Wi-Fi drops should be clearly larger than the noise.
4. `python3 scripts/power_fit.py power.csv` lists the runs; the fit appears once there are enough intervals.
