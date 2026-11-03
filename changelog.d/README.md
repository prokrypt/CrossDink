# Changelog fragments

One file per PR instead of editing `CHANGELOG.md`, so PRs stop conflicting on the Unreleased list.

`changelog.d/<branch-or-topic>.md`:

```
Fixed:
Sleep screen no longer shows the previous book's cover after a library delete.
```

First line: `Added:`, `Changed:`, `Deprecated:`, `Removed:`, `Fixed:` or `Security:`. The rest is the
entry (one bullet, user-facing, as in `CHANGELOG.md`). On every merge to main the `Changelog` workflow
runs `scripts/changelog_assemble.py`, which appends the fragments to their Unreleased sections, deletes
them and commits `chore: assemble changelog`. The PR check fails when a PR touches neither
`changelog.d/` nor `CHANGELOG.md` unless its body contains `No changelog`.

`python3 scripts/changelog_assemble.py --check` validates the fragments without writing.
