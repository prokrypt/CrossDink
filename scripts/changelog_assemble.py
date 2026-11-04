#!/usr/bin/env python3
"""Fold changelog.d/*.md fragments into the Unreleased section of CHANGELOG.md.

A fragment is one file per PR. Its first line names the section (`Added:`, `Changed:`,
`Deprecated:`, `Removed:`, `Fixed:` or `Security:`); the rest is the entry, which becomes
one bullet. Fragments are appended to the end of their section (created when missing) and
deleted. `--check` validates the fragments and prints the result without writing anything.
"""

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHANGELOG = ROOT / "CHANGELOG.md"
FRAGMENTS = ROOT / "changelog.d"
SECTIONS = ["Added", "Changed", "Deprecated", "Removed", "Fixed", "Security"]


def parse_fragment(path):
    head, _, body = path.read_text(encoding="utf-8").partition("\n")
    section = head.strip().rstrip(":").capitalize()
    if section not in SECTIONS or not head.strip().endswith(":"):
        sys.exit(f"{path}: first line must be one of {', '.join(s + ':' for s in SECTIONS)}")
    lines = body.strip().splitlines()
    if not lines:
        sys.exit(f"{path}: empty entry")
    if not lines[0].startswith("- "):
        lines[0] = "- " + lines[0]
    return section, lines


def assemble(text, fragments):
    """text: CHANGELOG.md; fragments: [(section, lines)] -> new text."""
    lines = text.splitlines()
    start = next(i for i, l in enumerate(lines) if l.startswith("## [Unreleased]")) + 1
    end = next((i for i in range(start, len(lines)) if lines[i].startswith("## ")), len(lines))
    for section in SECTIONS:
        entries = [l for s, ls in fragments if s == section for l in ls]
        if not entries:
            continue
        heads = [i for i in range(start, end) if lines[i].strip() == f"### {section}"]
        if heads:
            # Append after the last entry of the first matching block (duplicate headings exist).
            at = next((i for i in range(heads[0] + 1, end) if lines[i].startswith("###")), end)
            while at > heads[0] + 1 and not lines[at - 1].strip():
                at -= 1
            lines[at:at] = entries
            end += len(entries)
            continue
        # New section: before the first existing section that comes later in SECTIONS, else at the end.
        later = {f"### {s}" for s in SECTIONS[SECTIONS.index(section) + 1 :]}
        at = next((i for i in range(start, end) if lines[i].strip() in later), end)
        while at > start and not lines[at - 1].strip():
            at -= 1
        block = ["", f"### {section}", *entries]
        if at == end and at < len(lines):
            block.append("")
        lines[at:at] = block
        end += len(block)
    return "\n".join(lines) + "\n"


def self_test():
    text = "# Log\n\n## [Unreleased]\n\n### Added\n- a\n\n### Fixed\n- f\n\n## [1.0]\n\n### Added\n- old\n"
    out = assemble(text, [("Fixed", ["- g"]), ("Added", ["- b"]), ("Changed", ["- c"])])
    want = "# Log\n\n## [Unreleased]\n\n### Added\n- a\n- b\n\n### Changed\n- c\n\n### Fixed\n- f\n- g\n\n## [1.0]\n\n### Added\n- old\n"
    assert out == want, out


def main():
    check = "--check" in sys.argv
    self_test()
    paths = sorted(p for p in FRAGMENTS.glob("*.md") if p.name != "README.md")
    fragments = [parse_fragment(p) for p in paths]
    if not fragments:
        print("changelog_assemble: no fragments")
        return
    new = assemble(CHANGELOG.read_text(encoding="utf-8"), fragments)
    for p, (section, lines) in zip(paths, fragments):
        print(f"changelog_assemble: {p.name} -> {section}: {lines[0][:60]}")
    if check:
        return
    CHANGELOG.write_text(new, encoding="utf-8")
    for p in paths:
        p.unlink()


if __name__ == "__main__":
    main()
