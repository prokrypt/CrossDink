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

#ifndef CROSSDINK_GIT_SHA
#define CROSSDINK_GIT_SHA "unknown"
#endif

#ifndef CROSSDINK_GIT_DIRTY
#define CROSSDINK_GIT_DIRTY "unknown"
#endif

namespace BuildInfo {
const char* gitBranch() { return CROSSDINK_GIT_BRANCH; }
const char* shortBranch() { return CROSSDINK_GIT_BRANCH_SHORT; }
const char* buildNumber() { return CROSSDINK_BUILD_NUMBER; }
const char* buildTime() { return CROSSDINK_BUILD_TIME; }
const char* gitSha() { return CROSSDINK_GIT_SHA; }
const char* gitDirty() { return CROSSDINK_GIT_DIRTY; }
}  // namespace BuildInfo
