#pragma once

// Per-build provenance for the Settings > System footer. Kept out of
// AppVersion.h because scripts/git_branch.py defines these only for
// BuildInfo.cpp: the build time changes every build, and a global define would
// force a full rebuild each time.
namespace BuildInfo {
const char* gitBranch();
// Batch number ("b11") for batch branches, else the branch without its prefix folder.
const char* shortBranch();
const char* buildNumber();
const char* buildTime();
}  // namespace BuildInfo
