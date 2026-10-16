#!/usr/bin/env python3
"""Rewrites the DEFAULTS block of index.html from the C++ theme sources.

  python3 tools/theme-generator/sync_defaults.py          # rewrite
  python3 tools/theme-generator/sync_defaults.py --check  # exit 1 when stale

Field keys, kinds and ranges come from src/components/ThemePackFields.cpp, base
values from the *Metrics tables under src/components/themes/. Run it after either changes.
"""
import json, re, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src/components"
HTML = Path(__file__).with_name("index.html")


def read(p):
    return re.sub(r"//[^\n]*", "", (SRC / p).read_text())


def value(raw):
    raw = raw.strip()
    if raw in ("true", "false"):
        return raw == "true"
    if raw.startswith("ThemeTabBarAppearance::"):
        return {"Pill": 0, "BorderedText": 1}[raw.split("::")[1]]
    raw = raw.rstrip("f")
    return float(raw) if "." in raw else int(raw)


def table(text, name):
    block = text[text.index(name):]
    block = block[block.index("{") : block.index("};")]
    out = {}
    for k, v in re.findall(r"\.(\w+)\s*=\s*([^,}]+)", block):
        try:
            out[k] = value(v)
        except (ValueError, KeyError):
            pass  # StatusBarMetrics::... and other non-pack fields
    return out


def overrides(text):
    out = {}
    for k, op, v in re.findall(r"\bv\.(\w+)\s*([-+*/|&^]?=)\s*([^;]+);", text):
        if op != "=":
            raise SystemExit(f"v.{k} {op} not supported: write a plain assignment")
        out[k] = value(v)
    return out


fields_src = read("ThemePackFields.cpp")
types = dict((n, t) for t, n in re.findall(r"^\s*(int|bool|float|ThemeTabBarAppearance)\s+(\w+);", read("themes/BaseTheme.h"), re.M))
fields = []
for kind, name, rest in re.findall(r"^\s+TP_(FIELD|BOOL)\((\w+)(.*?)\),?\s*$", fields_src, re.M):
    t = types[name]
    if kind == "BOOL":
        fields.append([name, "bool"])
    else:
        lo, hi = (int(x, 0) for x in re.findall(r"-?(?:0x[0-9a-fA-F]+|\d+)", rest)[:2])
        fields.append([name, {"int": "int", "float": "ratio", "ThemeTabBarAppearance": "enum"}[t], lo, hi])
keys = [f[0] for f in fields]

lyra = table(read("themes/lyra/LyraTheme.h"), "LyraMetrics")
bases = {"classic": table(read("themes/BaseTheme.h"), "BaseMetrics"), "lyra": lyra}
bases["roundedraff"] = table(read("themes/roundedraff/RoundedRaffTheme.h"), "RoundedRaffMetrics")
bases["cover-grid"] = lyra
chain = [
    ("lyra-3-covers", "lyra", "themes/lyra/Lyra3CoversTheme.h"),
    ("lyra-carousel", "lyra", "themes/lyra/LyraCarouselTheme.h"),
    ("minimal", "lyra", "themes/minimal/MinimalTheme.h"),
    ("dashboard", "minimal", "themes/dashboard/DashboardTheme.h"),
]
for name, parent, path in chain:
    bases[name] = {**bases[parent], **overrides(read(path))}
bases = {b: {k: v[k] for k in keys} for b, v in bases.items()}  # KeyError: field missing from a table

text = "const FIELDS = %s;\nconst BASES = %s;" % (
    json.dumps(fields, separators=(",", ":")),
    json.dumps(bases, separators=(",", ":")),
)
html = HTML.read_text()
new = re.sub(r"(/\*DEFAULTS\*/\n).*?(\n/\*END\*/)", lambda m: m.group(1) + text + m.group(2), html, flags=re.S)
if "--check" in sys.argv:
    sys.exit(0 if new == html else "index.html defaults are stale: run sync_defaults.py")
HTML.write_text(new)
print(len(fields), "fields,", len(bases), "bases")
