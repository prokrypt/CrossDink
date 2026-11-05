"""Run with python3 -m unittest discover -s test/scripts (numpy required)."""
import csv
import io
import re
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import power_fit as pf  # noqa: E402


def firmware_header() -> list[str]:
    """kHeader from src/util/PowerLogRow.h: the columns the firmware writes."""
    src = (ROOT / "src/util/PowerLogRow.h").read_text()
    body = re.search(r"kHeader\[\]\s*=\s*((?:\s*\"[^\"]*\")+);", src).group(1)
    text = "".join(re.findall(r"\"([^\"]*)\"", body)).replace("\\n", "")
    return text.split(",")


HEADER = firmware_header()

# True cost of each feature, in the fit's units (%/h, or % per refresh).
TRUTH = {
    "sleep_h": 0.05,
    "sleep_cw_h": 0.09,
    "floor_h": 2.0,
    "ls_h": 0.6,
    "maxclk_h": 4.5,
    "wifi_ps_h": 3.0,
    "wifi_awake_h": 9.0,
    "light_full_h": 6.0,
    "ref_fast": 0.002,
    "ref_full": 0.012,
}


class Device:
    """Cumulative counters and a gauge that quantizes to 1/256 % with some wander."""

    def __init__(self, rng: np.random.Generator):
        self.rng = rng
        self.c = {k: 0.0 for k in pf.COUNTERS}
        self.soc = 95.0
        self.gen = 7
        self.epoch = 1_790_000_000
        self.uptime = 0
        self.rows: list[dict] = []

    def gauge(self) -> str:
        p = self.soc + self.rng.normal(0, 0.05)
        return f"{np.floor(max(p, 0) * 256) / 256:.2f}"

    def row(self, event: str, detail: str = "", charger: bool = False):
        rec = {k: "" for k in HEADER}
        rec.update(epoch_utc=str(self.epoch), uptime_ms=str(self.uptime), gen=str(self.gen), event=event,
                   detail=detail, pct=self.gauge(), chg="1" if charger else "0", usb="0",
                   chg_seen="1" if charger else "0", wifi="off")
        rec.update({k: str(int(round(v))) for k, v in self.c.items()})
        self.rows.append(rec)

    def awake(self, minutes: float, ls: float, maxclk: float, wifi: str | None, light: float, fast: int, full: int):
        ms = minutes * 60_000
        h = minutes / 60
        self.c["awake_ms"] += ms
        self.c["ls_ms"] += ms * ls
        self.c["maxclk_ms"] += ms * (1 - ls) * maxclk
        if wifi:
            self.c[f"wifi_{wifi}_ms"] += ms
        self.c["light_full_ms"] += ms * light
        self.c["ref_fast"] += fast
        self.c["ref_full"] += full
        floor = h * (1 - ls) * (1 - maxclk)
        drop = (TRUTH["floor_h"] * floor + TRUTH["ls_h"] * h * ls + TRUTH["maxclk_h"] * h * (1 - ls) * maxclk +
                TRUTH["light_full_h"] * h * light + TRUTH["ref_fast"] * fast + TRUTH["ref_full"] * full)
        if wifi:
            drop += TRUTH[f"wifi_{wifi}_h"] * h
        self.soc -= drop
        self.epoch += int(minutes * 60)
        self.uptime += int(ms)

    def sleep(self, hours: float, charger_wake: bool):
        key = "asleep_cw_s" if charger_wake else "asleep_s"
        self.c[key] += hours * 3600
        self.soc -= TRUTH["sleep_cw_h" if charger_wake else "sleep_h"] * hours
        self.epoch += int(hours * 3600)
        self.uptime = 0

    def charge(self):
        self.soc = 98.0
        self.epoch += 7200
        self.row("chg_off", charger=True)


def simulate(days: int = 40, seed: int = 3) -> list[dict]:
    rng = np.random.default_rng(seed)
    d = Device(rng)
    d.row("boot")
    for _ in range(days * 4):
        d.row("wake")
        for _ in range(int(rng.integers(1, 6))):
            wifi = rng.choice([None, None, None, "ps", "awake"])
            d.awake(minutes=float(rng.uniform(5, 15)), ls=float(rng.uniform(0, 0.9)),
                    maxclk=float(rng.uniform(0, 0.6)), wifi=wifi,
                    light=float(rng.uniform(0, 1)) if rng.random() < 0.5 else 0.0,
                    fast=int(rng.poisson(40)), full=int(rng.poisson(2)))
            d.row("tick")
        d.row("sleep")
        d.sleep(hours=float(rng.uniform(1, 6)), charger_wake=bool(rng.random() < 0.5))
        if d.soc < 15:
            d.charge()
    # A Power test pair: idle then light 100 %.
    d.row("test_start", "idle")
    d.awake(60, ls=0.8, maxclk=0.0, wifi=None, light=0.0, fast=0, full=0)
    d.row("test_end", "idle")
    d.row("test_start", "light100")
    d.awake(60, ls=0.8, maxclk=0.0, wifi=None, light=1.0, fast=0, full=0)
    d.row("test_end", "light100")
    return d.rows


def write(rows: list[dict]) -> str:
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False, newline="")
    w = csv.DictWriter(f, fieldnames=HEADER)
    w.writeheader()
    w.writerows(rows)
    f.close()
    return f.name


class HeaderTest(unittest.TestCase):
    def test_script_columns_are_written_by_the_firmware(self):
        missing = [c for c in pf.REQUIRED if c not in HEADER]
        self.assertEqual(missing, [])


class FitTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.path = write(simulate())
        cls.rows = pf.read_rows([cls.path])

    def test_recovers_the_costs(self):
        intervals, _ = pf.build_intervals(self.rows, 1.0)
        self.assertGreater(len(intervals), 100)
        names = list(TRUTH)
        res = pf.fit(intervals, names, folds=5, bootstrap=0, seed=1)
        got = dict(zip(res.names, res.coef))
        for name in ("sleep_h", "sleep_cw_h", "floor_h", "maxclk_h", "wifi_awake_h", "light_full_h"):
            self.assertAlmostEqual(got[name], TRUTH[name], delta=0.2 * TRUTH[name], msg=name)
        self.assertLess(res.cv_rmse, res.base_rmse)

    def test_intervals_skip_charging(self):
        intervals, skipped = pf.build_intervals(self.rows, 1.0)
        self.assertGreater(skipped["charger"], 0)
        for iv in intervals:
            self.assertGreater(iv.drop, 0)
            self.assertFalse(iv.b.charger)

    def test_intervals_stay_in_one_generation(self):
        rows = simulate(days=4)
        for r in rows[len(rows) // 2:]:
            r["gen"] = "8"
        parsed = pf.read_rows([write(rows)])
        intervals, skipped = pf.build_intervals(parsed, 0.5)
        self.assertEqual(skipped["gen"], 1)
        self.assertTrue(all(iv.a.gen == iv.b.gen for iv in intervals))

    def test_power_test_runs(self):
        runs, _ = pf.find_tests(self.rows)
        self.assertEqual([r.tag for r in runs], ["idle", "light100"])
        out = io.StringIO()
        pf.print_tests(runs, [], 2000, out)
        self.assertIn("light100", out.getvalue())
        self.assertIn("less the idle run", out.getvalue())

    def test_main_runs(self):
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(pf.main([self.path, "--capacity-mah", "2000", "--bootstrap", "20"]), 0)
        self.assertIn("mA", out.getvalue())


class ChargeAndRunTest(unittest.TestCase):
    def test_no_interval_starts_while_the_gauge_settles_after_a_charge(self):
        d = Device(np.random.default_rng(5))
        d.row("boot")
        d.charge()
        for _ in range(30):  # 5 min steps: the gauge's post-charge rise lands in the first 25 min
            d.awake(5, ls=0.5, maxclk=0.2, wifi=None, light=0.5, fast=10, full=0)
            d.row("tick")
        charger_row = next(r for r in pf.read_rows([write(d.rows)]) if r.charger)
        intervals, _ = pf.build_intervals(pf.read_rows([write(d.rows)]), 0.5, settle_min=30)
        self.assertTrue(intervals)
        for iv in intervals:
            self.assertGreaterEqual(pf.clock_s(iv.a) - pf.clock_s(charger_row), 30 * 60)

    def test_charger_in_the_middle_of_a_run_is_not_a_drain(self):
        d = Device(np.random.default_rng(6))
        d.row("test_start", "idle")
        d.awake(20, ls=0.8, maxclk=0, wifi=None, light=0, fast=0, full=0)
        d.row("pct", charger=True)  # plugged in and out between rows
        d.awake(20, ls=0.8, maxclk=0, wifi=None, light=0, fast=0, full=0)
        d.row("test_end", "idle")
        runs, _ = pf.find_tests(pf.read_rows([write(d.rows)]))
        self.assertEqual(len(runs), 1)
        self.assertTrue(runs[0].charger)
        out = io.StringIO()
        pf.print_tests(runs, [], None, out)
        self.assertIn("charger seen", out.getvalue())

    def test_short_stopped_run_is_left_out_of_the_comparison(self):
        d = Device(np.random.default_rng(7))
        d.row("test_start", "idle")
        d.awake(120, ls=0.8, maxclk=0, wifi=None, light=0, fast=0, full=0)
        d.row("test_end", "idle")
        d.row("test_start", "light100")
        d.awake(1, ls=0.8, maxclk=0, wifi=None, light=1, fast=0, full=0)  # stopped after a minute
        d.row("test_abort", "light100")
        runs, _ = pf.find_tests(pf.read_rows([write(d.rows)]))
        out = io.StringIO()
        pf.print_tests(runs, [], None, out)
        self.assertNotIn("less the idle run", out.getvalue())

    def test_overlapping_copies_are_read_once(self):
        rows = simulate(days=3)
        once = pf.read_rows([write(rows)])
        twice = pf.read_rows([write(rows), write(rows)])
        self.assertEqual(len(once), len(twice))


class NnlsTest(unittest.TestCase):
    def test_fallback_matches_unconstrained_when_positive(self):
        rng = np.random.default_rng(0)
        A = rng.uniform(0, 1, (50, 4))
        x = np.array([1.0, 2.0, 0.5, 3.0])
        np.testing.assert_allclose(pf._nnls(A, A @ x), x, rtol=1e-6)

    def test_fallback_clamps_negative(self):
        rng = np.random.default_rng(1)
        A = rng.uniform(0, 1, (50, 3))
        y = A @ np.array([1.0, -2.0, 1.0])
        got = pf._nnls(A, y)
        self.assertTrue((got >= 0).all())
        self.assertEqual(got[1], 0.0)


if __name__ == "__main__":
    unittest.main()
