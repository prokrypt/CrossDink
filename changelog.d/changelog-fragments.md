Changed:
Contributors: a PR now adds a `changelog.d/<topic>.md` fragment instead of editing `CHANGELOG.md`; a workflow folds the fragments into the Unreleased section on every merge to main, so PRs no longer conflict on the changelog. A PR without a fragment fails its Changelog check unless its description says `No changelog`.
