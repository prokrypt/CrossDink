#pragma once

// Per-build provenance (Settings > System footer, /api/status, battery log).
// Kept out of AppVersion.h because scripts/git_branch.py defines these only for
// BuildInfo.cpp: the build time, commit and dirty flag change between builds,
// and a global define would force a full rebuild each time.
namespace BuildInfo {
const char* gitBranch();
// Batch number ("b11") for batch branches, else the branch without its prefix folder.
const char* shortBranch();
const char* buildNumber();
const char* buildTime();
const char* gitSha();
// "1" when the build had tracked modifications, "0" when clean, else "unknown".
const char* gitDirty();
}  // namespace BuildInfo
