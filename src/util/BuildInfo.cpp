#include "BuildInfo.h"

#ifndef CROSSDINK_GIT_BRANCH
#define CROSSDINK_GIT_BRANCH "unknown"
#endif

#ifndef CROSSDINK_GIT_BRANCH_SHORT
#define CROSSDINK_GIT_BRANCH_SHORT CROSSDINK_GIT_BRANCH
#endif

#ifndef CROSSDINK_BUILD_NUMBER
#define CROSSDINK_BUILD_NUMBER ""
#endif

#ifndef CROSSDINK_BUILD_TIME
#define CROSSDINK_BUILD_TIME "unknown"
#endif

namespace BuildInfo {
const char* gitBranch() { return CROSSDINK_GIT_BRANCH; }
const char* shortBranch() { return CROSSDINK_GIT_BRANCH_SHORT; }
const char* buildNumber() { return CROSSDINK_BUILD_NUMBER; }
const char* buildTime() { return CROSSDINK_BUILD_TIME; }
}  // namespace BuildInfo
