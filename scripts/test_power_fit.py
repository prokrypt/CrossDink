#!/usr/bin/env python3
"""Self-check for power_fit.pool_runs: python3 scripts/test_power_fit.py"""
import os
import sys
from types import SimpleNamespace as NS

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from power_fit import MS_PER_H, pool_runs  # noqa: E402


def run(tag, pct0, pct1, hours, charger=False, aborted=False):
    return NS(tag=tag, charger=charger, aborted=aborted,
              start=NS(pct=pct0, c={"awake_ms": 0}), end=NS(pct=pct1, c={"awake_ms": hours * MS_PER_H}))


pooled = pool_runs([
    run("idle", 80, 79.8, 0.1),  # under 0.3 %: still pooled
    run("idle", 79.8, 79.5, 0.1),
    run("idle", 79.5, 79.4, 0.05, aborted=True),
    run("idle", 90, 95, 1.0, charger=True),  # excluded
    run("light50", 79, 78, 0.2),
])
drop, hours, n = pooled["idle"]
assert n == 3 and abs(drop - 0.6) < 1e-9 and abs(hours - 0.25) < 1e-9, pooled
assert pooled["light50"][2] == 1
print("ok")
