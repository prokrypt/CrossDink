#include "Activity.h"

#include <PerfLog.h>

#include "ActivityManager.h"
#include "FilenameFontSystem.h"

void Activity::onEnter() {
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
  PerfLog::noteActivity(name.c_str());
  // Load on the main task before the screen measures or renders translated
  // labels. The service is cache-first and refuses loads during SD handoff.
  filenameFontSystem.ensureLoaded(renderer);
}

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

RequestUpdateResult Activity::requestUpdateAndWait() { return activityManager.requestUpdateAndWait(); }

// cppcheck-suppress functionStatic ; per-instance navigation API, delegates to the ActivityManager singleton
void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

// cppcheck-suppress functionStatic ; per-instance navigation API, delegates to the ActivityManager singleton
void Activity::onSelectBook(const std::string& path) { activityManager.goToReader(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finishAfterBackPress() {
  mappedInput.suppressNextBackRelease();
  finish();
}

// cppcheck-suppress functionStatic ; per-instance navigation API, delegates to the ActivityManager singleton
void Activity::finish() { activityManager.popActivity(); }
