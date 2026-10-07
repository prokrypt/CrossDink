#!/usr/bin/env python3
"""Per-function battery drain from /debug/logs/power.csv (Goodies builds).

The X4 Pro's gauge only reports state of charge, so this fits the % drop over
many on-battery intervals as a sum of (activity in the interval) x (cost per
unit of that activity), with every cost >= 0 (non-negative least squares):

    drop % = sleep_h * a + floor_h * b + maxclk_h * c + light_full_h * d + ...

Each power.csv row carries cumulative counters (src/util/PowerLog.h), so the
activity in an interval is the difference between its end rows. Intervals
never cross a charge, USB power, a counter generation change (power loss,
crash, stats reset) or a counter that went backwards, and each one spans at
least --min-drop % so the gauge's own wander stays small next to the drop.

Power test runs (Goodies > Power test) are summarised separately: their drop
over their run, per hour, and against the idle run when there is one.

Usage:
    python3 scripts/power_fit.py power.csv power.1.csv --capacity-mah 2000

--capacity-mah turns %/h into mA (1 %/h = capacity / 100 mA). Use the cell's
rated capacity; the gauge's own capacity model is what the % is a share of.

Needs only numpy.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

# Cumulative columns: a difference between two rows is the activity between them.
COUNTERS = [
    "awake_ms", "asleep_s", "asleep_cw_s", "ls_ms", "maxclk_ms", "busy0_ms", "busy1_ms",
    "wifi_up_ms", "wifi_ps_ms", "wifi_awake_ms", "wifi_ap_ms", "scans", "connects", "ip_tx", "ip_rx",
    "light_full_ms", "ref_full", "ref_half", "ref_fast", "ref_gray", "ref_flash",
    "panel_full_ms", "panel_half_ms", "panel_fast_ms", "panel_gray_ms", "booster_ms",
    "sd_rd_kb", "sd_wr_kb", "sd_ms",
]
REQUIRED = ["gen", "event", "pct", "chg", "usb", "chg_seen"] + COUNTERS

MS_PER_H = 3_600_000.0


@dataclass
class Feature:
    name: str
    unit: str  # "h" (coefficient in %/h) or a count unit (coefficient in % per unit)
    help: str
    per: float = 1.0  # rate features: 1; count features: how many units the shown cost is for


# name -> (unit, description). The fit's columns; build_features() computes them.
FEATURES = {
    "sleep_h": Feature("sleep_h", "h", "deep sleep, charger wake off"),
    "sleep_cw_h": Feature("sleep_cw_h", "h", "deep sleep, charger wake on (ext0)"),
    "floor_h": Feature("floor_h", "h", "awake at the DFS floor clock, not light-sleeping"),
    "ls_h": Feature("ls_h", "h", "automatic light sleep"),
    "maxclk_h": Feature("maxclk_h", "h", "awake with the full-clock lock held"),
    "busy_h": Feature("busy_h", "h", "core time outside the idle task (both cores added)"),
    "wifi_up_h": Feature("wifi_up_h", "h", "radio on, not associated"),
    "wifi_ps_h": Feature("wifi_ps_h", "h", "associated, modem sleep"),
    "wifi_awake_h": Feature("wifi_awake_h", "h", "associated, power save off"),
    "wifi_ap_h": Feature("wifi_ap_h", "h", "soft AP"),
    "wifi_kpkt": Feature("wifi_kpkt", "kpkt", "IP packets sent + received, thousands", 1.0),
    "light_full_h": Feature("light_full_h", "h", "frontlight, as hours at full duty"),
    "ref_full": Feature("ref_full", "refresh", "Full refreshes", 100.0),
    "ref_half": Feature("ref_half", "refresh", "Half refreshes", 100.0),
    "ref_fast": Feature("ref_fast", "refresh", "Fast refreshes", 100.0),
    "ref_gray": Feature("ref_gray", "refresh", "gray passes", 100.0),
    "booster_h": Feature("booster_h", "h", "panel booster on between refreshes (UC8179)"),
    "sd_mb": Feature("sd_mb", "MB", "SD card read + written", 1.0),
}
DEFAULT_FEATURES = [
    "sleep_h", "sleep_cw_h", "floor_h", "ls_h", "maxclk_h", "wifi_up_h", "wifi_ps_h", "wifi_awake_h",
    "wifi_ap_h", "light_full_h", "ref_full", "ref_half", "ref_fast", "ref_gray",
]


@dataclass
class Row:
    line: int
    file: str
    gen: str
    event: str
    detail: str
    pct: float | None
    charger: bool  # charging or USB at the row, or seen since the previous row
    epoch: int | None
    uptime_ms: int
    c: dict = field(default_factory=dict)


def read_rows(paths: list[str]) -> list[Row]:
    """Rows of every file, oldest file first (power.1.csv before power.csv)."""
    def age_key(p: str) -> tuple:
        name = Path(p).name
        parts = name.split(".")
        # power.1.csv -> 1 (older), power.csv -> 0
        n = int(parts[1]) if len(parts) == 3 and parts[1].isdigit() else 0
        return (-n, p)

    rows: list[Row] = []
    seen: set[tuple] = set()  # the same row from two overlapping copies is kept once
    for path in sorted(paths, key=age_key):
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            reader = csv.DictReader(f)
            missing = [c for c in REQUIRED if c not in (reader.fieldnames or [])]
            if missing:
                raise SystemExit(f"{path}: not a power.csv (missing {', '.join(missing)})")
            for i, rec in enumerate(reader, start=2):
                try:
                    counters = {k: float(rec[k] or 0) for k in COUNTERS}
                except ValueError:
                    continue  # a torn row (power cut mid-write)
                key = (rec["gen"], rec["event"], rec.get("epoch_utc"), rec.get("uptime_ms"), rec["awake_ms"])
                if key in seen:
                    continue
                seen.add(key)
                pct = float(rec["pct"]) if rec["pct"] else None
                rows.append(Row(
                    line=i, file=path, gen=rec["gen"], event=rec["event"], detail=rec.get("detail", ""),
                    pct=pct,
                    charger=rec["chg"] == "1" or rec["usb"] == "1" or rec["chg_seen"] == "1",
                    epoch=int(rec["epoch_utc"]) if rec.get("epoch_utc") else None,
                    uptime_ms=int(rec.get("uptime_ms") or 0), c=counters))
    return rows


def features_of(a: Row, b: Row) -> dict[str, float]:
    """Activity between rows a and b, in the fit's units."""
    d = {k: b.c[k] - a.c[k] for k in COUNTERS}
    awake = d["awake_ms"]
    ls = min(d["ls_ms"], awake)
    maxclk = min(d["maxclk_ms"], awake - ls)
    return {
        "sleep_h": d["asleep_s"] / 3600.0,
        "sleep_cw_h": d["asleep_cw_s"] / 3600.0,
        "floor_h": max(awake - ls - maxclk, 0.0) / MS_PER_H,
        "ls_h": ls / MS_PER_H,
        "maxclk_h": maxclk / MS_PER_H,
        "busy_h": (d["busy0_ms"] + d["busy1_ms"]) / MS_PER_H,
        "wifi_up_h": d["wifi_up_ms"] / MS_PER_H,
        "wifi_ps_h": d["wifi_ps_ms"] / MS_PER_H,
        "wifi_awake_h": d["wifi_awake_ms"] / MS_PER_H,
        "wifi_ap_h": d["wifi_ap_ms"] / MS_PER_H,
        "wifi_kpkt": (d["ip_tx"] + d["ip_rx"]) / 1000.0,
        "light_full_h": d["light_full_ms"] / MS_PER_H,
        "ref_full": d["ref_full"],
        "ref_half": d["ref_half"],
        "ref_fast": d["ref_fast"],
        "ref_gray": d["ref_gray"],
        "booster_h": d["booster_ms"] / MS_PER_H,
        "sd_mb": (d["sd_rd_kb"] + d["sd_wr_kb"]) / 1024.0,
        "_hours": (awake / 1000.0 + d["asleep_s"] + d["asleep_cw_s"]) / 3600.0,
    }


def clock_s(r: Row) -> float:
    """Seconds of awake plus asleep time: a clock that runs within one generation."""
    return r.c["awake_ms"] / 1000.0 + r.c["asleep_s"] + r.c["asleep_cw_s"]


def counters_ok(a: Row, b: Row) -> bool:
    return all(b.c[k] >= a.c[k] for k in COUNTERS)


@dataclass
class Interval:
    a: Row
    b: Row
    drop: float
    x: dict


def build_intervals(rows: list[Row], min_drop: float,
                    settle_min: float = 30.0) -> tuple[list[Interval], dict[str, int]]:
    """Back-to-back on-battery intervals of at least min_drop %.

    No interval starts within settle_min minutes (awake or asleep) of a row that
    saw a charger: the gauge keeps rising for about 25 min after unplugging, which
    would make those intervals read too little drop.
    """
    intervals: list[Interval] = []
    skipped = {"charger": 0, "gen": 0, "backwards": 0, "rise": 0}
    anchor: Row | None = None
    prev: Row | None = None
    last_charger: Row | None = None

    def settled(r: Row) -> bool:
        if r.charger:
            return False
        if last_charger is None or last_charger.gen != r.gen:
            return True
        return clock_s(r) - clock_s(last_charger) >= settle_min * 60

    for r in rows:
        if r.charger:
            last_charger = r
        if r.pct is None:
            prev = r
            continue
        if anchor is not None and prev is not None:
            reason = None
            if r.gen != anchor.gen:
                reason = "gen"
            elif r.charger:
                reason = "charger"
            elif not counters_ok(prev, r):
                reason = "backwards"
            elif r.pct > anchor.pct + 0.5:
                reason = "rise"  # charged without a row saying so
            if reason:
                skipped[reason] += 1
                anchor = r if settled(r) else None
                prev = r
                continue
        if anchor is None:
            anchor = r if settled(r) else None
            prev = r
            continue
        if anchor.pct - r.pct >= min_drop:
            intervals.append(Interval(anchor, r, anchor.pct - r.pct, features_of(anchor, r)))
            anchor = r
        prev = r
    return intervals, skipped


def nnls(A: np.ndarray, y: np.ndarray, max_iter: int = 0) -> np.ndarray:
    """min ||A x - y|| subject to x >= 0: scipy's when installed, else Lawson-Hanson here."""
    try:
        from scipy.optimize import nnls as scipy_nnls
    except ImportError:
        return _nnls(A, y, max_iter)
    return scipy_nnls(A, y)[0]


def _nnls(A: np.ndarray, y: np.ndarray, max_iter: int = 0) -> np.ndarray:
    m, n = A.shape
    max_iter = max_iter or 3 * n + 30
    x = np.zeros(n)
    passive = np.zeros(n, dtype=bool)
    w = A.T @ (y - A @ x)
    tol = 10 * np.finfo(float).eps * np.linalg.norm(A, 1) * max(m, n)
    it = 0
    while not passive.all() and (w[~passive] > tol).any() and it < max_iter:
        it += 1
        j = np.argmax(np.where(passive, -np.inf, w))
        passive[j] = True
        while True:
            z = np.zeros(n)
            z[passive] = np.linalg.lstsq(A[:, passive], y, rcond=None)[0]
            if (z[passive] > 0).all():
                x = z
                break
            neg = passive & (z <= 0)
            denom = x[neg] - z[neg]
            ratios = np.divide(x[neg], denom, out=np.zeros_like(denom), where=denom > 0)
            alpha = float(ratios.min()) if ratios.size else 0.0
            x = x + alpha * (z - x)
            passive &= x > tol
            x[~passive] = 0
        w = A.T @ (y - A @ x)
    return x


@dataclass
class Fit:
    names: list[str]
    coef: np.ndarray
    lo: np.ndarray
    hi: np.ndarray
    share: np.ndarray
    rmse: float
    cv_rmse: float
    base_rmse: float
    n: int


def fit(intervals: list[Interval], names: list[str], folds: int, bootstrap: int, seed: int) -> Fit | None:
    X = np.array([[iv.x[k] for k in names] for iv in intervals], dtype=float)
    y = np.array([iv.drop for iv in intervals], dtype=float)
    hours = np.array([iv.x["_hours"] for iv in intervals], dtype=float)
    keep = X.std(axis=0) > 0
    names = [n for n, k in zip(names, keep) if k]
    X = X[:, keep]
    if len(y) < max(len(names) + 2, 4):
        return None
    # Column scaling keeps the active-set solver well conditioned.
    scale = np.where(X.max(axis=0) > 0, X.max(axis=0), 1.0)
    coef = nnls(X / scale, y) / scale
    rmse = float(np.sqrt(np.mean((X @ coef - y) ** 2)))

    rng = np.random.default_rng(seed)
    order = rng.permutation(len(y))
    k = max(2, min(folds, len(y)))
    err, base_err = [], []
    for f in range(k):
        test = order[f::k]
        train = np.setdiff1d(order, test)
        c = nnls(X[train] / scale, y[train]) / scale
        err.extend((X[test] @ c - y[test]) ** 2)
        # Baseline: one average drain per hour of any kind.
        rate = y[train].sum() / max(hours[train].sum(), 1e-9)
        base_err.extend((rate * hours[test] - y[test]) ** 2)

    boots = []
    for _ in range(bootstrap):
        idx = rng.integers(0, len(y), len(y))
        boots.append(nnls(X[idx] / scale, y[idx]) / scale)
    boots = np.array(boots) if boots else np.array([coef])
    lo, hi = np.percentile(boots, 5, axis=0), np.percentile(boots, 95, axis=0)
    share = coef * X.sum(axis=0) / max(y.sum(), 1e-9)
    return Fit(names, coef, lo, hi, share, rmse, float(np.sqrt(np.mean(err))), float(np.sqrt(np.mean(base_err))),
               len(y))


def fmt_cost(name: str, value: float, capacity: float | None) -> str:
    f = FEATURES[name]
    if f.unit == "h":
        s = f"{value:8.3f} %/h"
        if capacity:
            s += f"  {value * capacity / 100:8.2f} mA"
        return s
    per = f.per
    s = f"{value * per:8.4f} %/{per:g} {f.unit}" if per != 1 else f"{value:8.4f} %/{f.unit}"
    if capacity:
        s += f"  {value * per * capacity / 100:8.2f} mAh/{per:g}"
    return s


def print_fit(res: Fit, capacity: float | None, out) -> None:
    print(f"Fit over {res.n} intervals. RMSE {res.rmse:.3f} %; cross-validated {res.cv_rmse:.3f} % "
          f"(one average rate per hour: {res.base_rmse:.3f} %)", file=out)
    print(file=out)
    width = max(len(n) for n in res.names)
    for name, c, lo, hi, share in zip(res.names, res.coef, res.lo, res.hi, res.share):
        per = FEATURES[name].per
        span = f"[{lo * per:.3g} .. {hi * per:.3g}]"
        print(f"  {name:<{width}}  {fmt_cost(name, c, capacity)}   90% {span:<22} {share * 100:5.1f}% of drain"
              f"   {FEATURES[name].help}", file=out)
    print(file=out)
    print("A cost of 0 means the data could not tell it apart from the others (or it really is free).", file=out)
    print("A wide 90% range means too few intervals vary it: run its Power test.", file=out)


@dataclass
class TestRun:
    tag: str
    start: Row
    end: Row
    aborted: bool
    charger: bool  # charging or USB seen at any row of the run


def find_tests(rows: list[Row]) -> tuple[list[TestRun], list[Row]]:
    runs, sags = [], []
    open_run: Row | None = None
    charged = False
    for r in rows:
        if r.event == "test_start":
            open_run = r
            charged = r.charger
            continue
        charged = charged or r.charger
        if r.event in ("test_end", "test_abort") and open_run is not None:
            if r.gen == open_run.gen and r.detail == open_run.detail:
                runs.append(TestRun(r.detail, open_run, r, r.event == "test_abort", charged))
            open_run = None
        elif r.event == "sag":
            sags.append(r)
        elif r.event in ("sleep", "wake", "boot"):
            open_run = None
    return runs, sags


def print_tests(runs: list[TestRun], sags: list[Row], capacity: float | None, out) -> None:
    if not runs and not sags:
        return
    print("Power test runs:", file=out)
    rates: dict[str, list[float]] = {}
    for t in runs:
        hours = (t.end.c["awake_ms"] - t.start.c["awake_ms"]) / MS_PER_H
        if t.start.pct is None or t.end.pct is None or hours <= 0:
            continue
        drop = t.start.pct - t.end.pct
        rate = drop / hours
        flag = ""
        usable = False
        if t.charger:
            flag = "  (charger seen: not a drain)"
        elif drop < 0.3:
            flag = "  (under 0.3 %: gauge wander dominates)"
        else:
            usable = True  # a run stopped early still measured its own load
            if t.aborted:
                flag = "  (stopped early)"
        if usable:
            rates.setdefault(t.tag, []).append(rate)
        ma = f"  {rate * capacity / 100:7.1f} mA" if capacity else ""
        print(f"  {t.tag:<11} {hours * 60:6.1f} min  -{drop:5.2f} %  {rate:6.2f} %/h{ma}{flag}", file=out)
    idle = np.mean(rates["idle"]) if rates.get("idle") else None
    if idle is not None and len(rates) > 1:
        print(file=out)
        print("  Each run's average less the idle run's average (the load's own cost):", file=out)
        for tag, rs in sorted(rates.items()):
            if tag == "idle":
                continue
            extra = float(np.mean(rs)) - idle
            ma = f"  {extra * capacity / 100:+7.1f} mA" if capacity else ""
            print(f"    {tag:<11} {extra:+6.2f} %/h{ma}", file=out)
    if sags:
        print(file=out)
        print("Voltage sag probe (cell mV under each load less without; larger drop = more current):", file=out)
        for s in sags:
            print(f"  {s.detail}", file=out)
    print(file=out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("files", nargs="+", help="power.csv and its rotated copy power.1.csv")
    ap.add_argument("--capacity-mah", type=float, help="cell capacity, to show mA beside %%/h")
    ap.add_argument("--min-drop", type=float, default=1.0, help="smallest %% drop per interval (default 1.0)")
    ap.add_argument("--settle-min", type=float, default=30.0,
                    help="minutes after a charge before an interval may start (default 30)")
    ap.add_argument("--features", help="comma-separated fit columns (default: %s)" % ",".join(DEFAULT_FEATURES))
    ap.add_argument("--all-features", action="store_true", help="fit every column, busy/packets/booster/SD included")
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--bootstrap", type=int, default=200, help="resamples for the 90%% ranges (0 = none)")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--intervals-csv", help="also write each interval's drop and features here")
    args = ap.parse_args(argv)

    names = DEFAULT_FEATURES
    if args.all_features:
        names = list(FEATURES)
    if args.features:
        names = [n.strip() for n in args.features.split(",") if n.strip()]
        unknown = [n for n in names if n not in FEATURES]
        if unknown:
            ap.error(f"unknown features: {', '.join(unknown)} (known: {', '.join(FEATURES)})")

    rows = read_rows(args.files)
    out = sys.stdout
    print(f"{len(rows)} rows, {len({r.gen for r in rows})} counter generation(s)", file=out)
    runs, sags = find_tests(rows)
    print_tests(runs, sags, args.capacity_mah, out)

    intervals, skipped = build_intervals(rows, args.min_drop, args.settle_min)
    hours = sum(iv.x["_hours"] for iv in intervals)
    print(f"{len(intervals)} intervals of >= {args.min_drop:g} % over {hours:.1f} h "
          f"(breaks: {skipped['charger']} charger, {skipped['gen']} generation, "
          f"{skipped['backwards']} counter reset, {skipped['rise']} unexplained rise)", file=out)
    if args.intervals_csv:
        with open(args.intervals_csv, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["from_line", "to_line", "drop"] + list(FEATURES))
            for iv in intervals:
                w.writerow([iv.a.line, iv.b.line, f"{iv.drop:.3f}"] + [f"{iv.x[k]:.5f}" for k in FEATURES])
    res = fit(intervals, names, args.folds, args.bootstrap, args.seed)
    if res is None:
        print("Not enough intervals to fit yet: keep logging, or lower --min-drop.", file=out)
        return 1
    print(file=out)
    print_fit(res, args.capacity_mah, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
