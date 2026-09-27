#include "BuildInfo.h"

#ifndef CROSSINK_GIT_BRANCH
#define CROSSINK_GIT_BRANCH "unknown"
#endif

#ifndef CROSSINK_GIT_BRANCH_SHORT
#define CROSSINK_GIT_BRANCH_SHORT CROSSINK_GIT_BRANCH
#endif

#ifndef CROSSINK_BUILD_NUMBER
#define CROSSINK_BUILD_NUMBER ""
#endif

#ifndef CROSSINK_BUILD_TIME
#define CROSSINK_BUILD_TIME "unknown"
#endif

namespace BuildInfo {
const char* gitBranch() { return CROSSINK_GIT_BRANCH; }
const char* shortBranch() { return CROSSINK_GIT_BRANCH_SHORT; }
const char* buildNumber() { return CROSSINK_BUILD_NUMBER; }
const char* buildTime() { return CROSSINK_BUILD_TIME; }
}  // namespace BuildInfo
