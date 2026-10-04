#include "ActivityManager.h"

#if CROSSDINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#endif

#include <CrossDinkHalFrontlight.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PerfLog.h>
#include <TaskCores.h>
#include <Xtc.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SilentRestart.h"
#include "boot_sleep/BootActivity.h"
#if CROSSDINK_GOODIES
#include "goodies/GoodiesActivity.h"
#endif
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "components/ListSelection.h"
#include "components/TouchRegistry.h"
#include "components/themes/BaseTheme.h"
#include "home/AlertActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBookProgress.h"
#include "library/LibraryActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "network/NearbyBookTransferActivity.h"
#include "network/NearbyStatsSyncActivity.h"
#include "network/UsbDriveActivity.h"
#include "network/WifiBackgroundJoin.h"
#include "platform/InputTask.h"
#include "reader/BookReadingStats.h"
#include "reader/BookStatsActivity.h"
#include "reader/BookStatsTracking.h"
#include "reader/GlobalReadingStats.h"
#include "reader/KOSyncAuto.h"
#include "reader/ReaderActivity.h"
#include "reader/ReaderExitSave.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/BatteryLog.h"
#include "util/FrontlightPanelActivity.h"
#include "util/FullScreenMessageActivity.h"
#include "util/SwipeAdjustment.h"
#include "util/TransferLightPulse.h"
#include "util/TwoFingerSwipe.h"

namespace {
constexpr uint32_t FILE_TRANSFER_MODE_MASK = 0xFF;
constexpr uint32_t FILE_TRANSFER_RETURN_TO_READER = 1U << 8;

constexpr bool hasStickyReaderDetailsPanel() {
#if defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  return true;
#else
  return false;
#endif
}

uint32_t fileTransferBootPayload(const NetworkMode mode, const bool returnToReader) {
  return static_cast<uint32_t>(mode) | (returnToReader ? FILE_TRANSFER_RETURN_TO_READER : 0);
}

void restartToFileTransfer(const NetworkMode mode, const std::string& returnBookPath) {
  silentRestartToNetwork(NetworkBootTarget::FILE_TRANSFER, fileTransferBootPayload(mode, !returnBookPath.empty()));
}

std::string fileNameFromPath(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

bool isNearbyTransferFile(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path);
}

FrontlightPanelContext buildFrontlightPanelContext(Activity& activity, GfxRenderer& renderer,
                                                   MappedInputManager& mappedInput) {
  FrontlightPanelContext context;
  context.sourceActivity = &activity;
  const std::string currentPath = activity.getCurrentBookPath();
  const bool currentBookValid = isNearbyTransferFile(currentPath) && Storage.exists(currentPath.c_str());
  const bool currentEpubValid = FsHelpers::hasEpubExtension(currentPath) && currentBookValid;
  const bool lastValid = !APP_STATE.openEpubPath.empty() && FsHelpers::hasEpubExtension(APP_STATE.openEpubPath) &&
                         Storage.exists(APP_STATE.openEpubPath.c_str());
  context.activeReaderBook = hasFrontlightActiveReaderBook(activity.isReaderActivity(), currentBookValid);
  context.showReadingStatsAction = SETTINGS.shouldTrackReadingStats();
  if (context.activeReaderBook) {
    if (FsHelpers::hasEpubExtension(currentPath)) {
      context.showReadingStatsAction =
          BookStatsTracking::isEnabled(Epub::cachePathForFilePath(currentPath, "/.crossdink"));
    } else if (FsHelpers::hasXtcExtension(currentPath)) {
      context.showReadingStatsAction = BookStatsTracking::isEnabled(Xtc(currentPath, "/.crossdink").getCachePath());
    }
    context.bookTitle = activity.getCurrentBookTitle();
    context.bookPath = currentPath;
    context.activeEpub = activity.isEpubReaderActivity() && currentEpubValid;
    if (activity.getFrontlightPanelBookDetails(context.bookDetails)) {
      context.showReaderDetails =
          shouldShowStickyReaderDetails(hasStickyReaderDetailsPanel(), Frontlight.present(), context.activeReaderBook);
      context.bookTitle = context.bookDetails.title;
    }
    if (context.showReadingStatsAction) context.readingStatsActivity = activity.createFrontlightReadingStatsActivity();
    if (context.activeEpub) {
      context.bookPath = currentPath;
      return context;
    }
    if (context.readingStatsActivity) return context;
  }

  const FrontlightBookSource source = chooseFrontlightBookSource(false, false, lastValid && !context.activeReaderBook);

  if (!SETTINGS.shouldTrackReadingStats()) return context;
  const GlobalReadingStats global = GlobalReadingStats::load();
  std::string cachePath;
  std::string statsTitle;
  BookReadingStats bookStats;
  float progress = -1.0f;
  if (source == FrontlightBookSource::LastBook) {
    context.bookPath = APP_STATE.openEpubPath;
    context.bookTitle = fileNameFromPath(context.bookPath);
    statsTitle = context.bookTitle;
    cachePath = Epub::cachePathForFilePath(context.bookPath, "/.crossdink");
    if (BookStatsTracking::isBookEnabled(cachePath))
      bookStats = BookReadingStats::load(cachePath);
    else {
      cachePath.clear();
      statsTitle = tr(STR_READING_STATS);
    }
    const RecentBook book{context.bookPath, context.bookTitle, {}, {}};
    progress = RecentBookProgress::loadCachedEpubPercent(book);
  } else {
    statsTitle = tr(STR_READING_STATS);
    if (!context.activeReaderBook) context.bookTitle = statsTitle;
  }
  if (GlobalReadingStats::hasSyncedStats()) {
    context.readingStatsActivity =
        makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, statsTitle, cachePath, bookStats, progress, false,
                                             0, global, GlobalReadingStats::loadAggregated(global));
  } else {
    context.readingStatsActivity = makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, statsTitle, cachePath,
                                                                        bookStats, progress, false, 0, global);
  }
  return context;
}

bool openFrontlightPanel(Activity& activity, GfxRenderer& renderer, MappedInputManager& mappedInput,
                         const FrontlightDrawerState* restoredState = nullptr) {
  FrontlightPanelContext context = buildFrontlightPanelContext(activity, renderer, mappedInput);
  if (restoredState) context.drawerState = *restoredState;
  auto panel = makeUniqueNoThrow<FrontlightPanelActivity>(renderer, mappedInput, std::move(context));
  if (!panel) {
    LOG_ERR("ACT", "OOM opening frontlight panel");
    return false;
  }
  TransferLightPulse::yieldToUser();  // the pulldown shows the user's level, so the LEDs do too
  activity.onFrontlightPanelOpened();
  activity.startActivityForResult(std::move(panel), [&activity](const ActivityResult& result) {
    const auto* panelResult = std::get_if<FrontlightPanelResult>(&result.data);
    if (panelResult) activity.handleFrontlightPanelResult(*panelResult);
  });
  return true;
}

// A brightness gesture that ends at 0% turns the light off and keeps `level`
// (the level before the gesture) for the next on. PWM is 0 at 0% either way.
void lightOffAtZero(const uint8_t level) {
  if (!Frontlight.isOn() || Frontlight.brightness() != 0) return;
  Frontlight.setBrightness(level);
  Frontlight.setOn(false);
  SETTINGS.frontlightBrightness = level;
  SETTINGS.frontlightOn = 0;
}

bool applyConfiguredSwipeAction(Activity& activity, ActivityManager& activityManager, const uint8_t action,
                                const int lightAmount = 5, const bool persist = true) {
  switch (static_cast<CrossPointSettings::TWO_FINGER_SWIPE_ACTION>(action)) {
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS:
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS: {
      if (!Frontlight.present()) return true;
      const uint8_t previousBrightness = Frontlight.brightness();
      const bool previousOn = Frontlight.isOn();
      const int delta = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ? lightAmount : -lightAmount;
      const uint8_t brightness =
          static_cast<uint8_t>(std::clamp(static_cast<int>(Frontlight.brightness()) + delta, 0, 100));
      Frontlight.setBrightness(brightness);
      Frontlight.setOn(true);
      SETTINGS.frontlightBrightness = brightness;
      SETTINGS.frontlightOn = 1;
      if (persist) lightOffAtZero(previousBrightness);  // a live slide does this at its end
      activity.onExternalFrontlightChange();
      if (persist && (brightness != previousBrightness || !previousOn)) activityManager.persistGlobalSettings();
      return true;
    }
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH:
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH: {
      if (!Frontlight.present() || !Frontlight.hasColorTemperature()) return true;
      const uint8_t previousWarmth = Frontlight.warmth();
      const bool previousOn = Frontlight.isOn();
      const int delta = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH ? lightAmount : -lightAmount;
      const uint8_t warmth = static_cast<uint8_t>(std::clamp(static_cast<int>(Frontlight.warmth()) + delta, 0, 100));
      Frontlight.setWarmth(warmth);
      SETTINGS.frontlightWarmth = warmth;
      SETTINGS.frontlightOn = Frontlight.isOn() ? 1 : 0;
      activity.onExternalFrontlightChange();
      if (persist && (warmth != previousWarmth || Frontlight.isOn() != previousOn))
        activityManager.persistGlobalSettings();
      return true;
    }
    case CrossPointSettings::TWO_FINGER_SWIPE_NEXT_CHAPTER:
    case CrossPointSettings::TWO_FINGER_SWIPE_PREVIOUS_CHAPTER:
    case CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_FONT_SIZE:
    case CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_FONT_SIZE:
      activity.handleTwoFingerSwipeAction(static_cast<CrossPointSettings::TWO_FINGER_SWIPE_ACTION>(action));
      return true;
    case CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET:
    case CrossPointSettings::TWO_FINGER_SWIPE_ACTION_COUNT:
      return false;
  }
  return true;
}

bool isLightSwipeAction(const uint8_t action) {
  return action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
         action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS ||
         action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH ||
         action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_WARMTH;
}

#if CROSSDINK_APP_CAP_TOUCH
void finishLiveLightSwipe(LiveLightSwipeState& state, ActivityManager& activityManager) {
  // The light stays at the level the slide last showed; only 0% turns it off.
  const bool brightness = state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                          state.action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS;
  if (brightness && Frontlight.isOn() && Frontlight.brightness() == 0) {
    lightOffAtZero(state.initialValue);
    if (state.owner) state.owner->onExternalFrontlightChange();
    state.changed = true;
  }
  if (state.changed) activityManager.persistGlobalSettings();
  state = {};
}

void updateLiveLightSwipe(Activity& activity, ActivityManager& activityManager, LiveLightSwipeState& state,
                          const int amount) {
  const bool brightness = state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                          state.action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS;
  const int sign = state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                           state.action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_WARMTH
                       ? 1
                       : -1;
  const int target = SwipeAdjustment::targetValue(state.initialValue, sign > 0, amount);
  const int current = brightness ? Frontlight.brightness() : Frontlight.warmth();
  const int difference = sign * (target - current);
  if (difference != 0 || (brightness && amount != 0 && !Frontlight.isOn())) {
    // setBrightness() ends a dim; a slide mid-flash keeps the flash duck on the new level.
    const uint8_t duck = Frontlight.idleDimPercent();
    applyConfiguredSwipeAction(activity, activityManager, state.action, difference, false);
    if (duck < 100 && Frontlight.isOn()) Frontlight.setIdleDim(duck);
  }
  // Without a dead zone, amount 0 is only a narrow band mid-drag, so restoring
  // the initial on/off state here would blink the light off and back on while
  // reversing through it. cancelLiveLightSwipe() restores it when needed.
  state.changed = (brightness ? Frontlight.brightness() : Frontlight.warmth()) != state.initialValue ||
                  Frontlight.isOn() != state.initialOn;
}

void cancelLiveLightSwipe(Activity& activity, ActivityManager& activityManager, LiveLightSwipeState& state) {
  updateLiveLightSwipe(activity, activityManager, state, 0);
  if (Frontlight.isOn() != state.initialOn) {
    Frontlight.setOn(state.initialOn);
    SETTINGS.frontlightOn = state.initialOn ? 1 : 0;
    activity.onExternalFrontlightChange();
  }
  state.active = false;
  state.blocked = true;
  state.changed = false;
}
#endif

uint8_t actionForTwoFingerDirection(const TwoFingerSwipe::Direction direction) {
  switch (direction) {
    case TwoFingerSwipe::Direction::Up:
      return SETTINGS.twoFingerSwipeUp;
    case TwoFingerSwipe::Direction::Down:
      return SETTINGS.twoFingerSwipeDown;
    case TwoFingerSwipe::Direction::Left:
      return SETTINGS.twoFingerSwipeLeft;
    case TwoFingerSwipe::Direction::Right:
      return SETTINGS.twoFingerSwipeRight;
    case TwoFingerSwipe::Direction::None:
      return CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET;
  }
  return CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET;
}

#if CROSSDINK_APP_CAP_TOUCH
bool applyLiveTwoFingerLightSwipe(Activity& activity, MappedInputManager& mappedInput, const GfxRenderer& renderer,
                                  ActivityManager& activityManager, LiveLightSwipeState& state,
                                  LiveLightSwipeState& edgeState) {
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  if (!mappedInput.getTwoFingerTouch(x1, y1, x2, y2)) {
    if (!state.tracking) return false;
    const bool consumed = state.active;
    if (consumed) {
      MappedInputManager::CompletedRotation rotation;
      if (mappedInput.wasCompletedMultiTouchRotation(rotation)) {
        cancelLiveLightSwipe(activity, activityManager, state);
        state = {};
        return false;  // Let the SDK's completed rotation keep its priority.
      }
      finishLiveLightSwipe(state, activityManager);
    } else {
      state = {};
    }
    return consumed;
  }

  mappedInput.resetEdgeSlide();
  if (edgeState.active) finishLiveLightSwipe(edgeState, activityManager);
  const int centerX = (x1 + x2) / 2;
  const int centerY = (y1 + y2) / 2;
  const TwoFingerSwipe::FingerPair rawCurrent = {x1, y1, x2, y2};
  if (!state.tracking || state.owner != &activity) {
    state = {};
    state.owner = &activity;
    state.tracking = true;
    state.startX = centerX;
    state.startY = centerY;
    state.firstX = x1;
    state.firstY = y1;
    state.secondX = x2;
    state.secondY = y2;
    return false;
  }

  if (state.blocked) return false;
  const TwoFingerSwipe::FingerPair start = {state.firstX, state.firstY, state.secondX, state.secondY};
  const TwoFingerSwipe::FingerPair current = TwoFingerSwipe::alignedPair(start, rawCurrent);
  if (!TwoFingerSwipe::hasTranslationGeometry(start, current)) {
    if (state.active)
      cancelLiveLightSwipe(activity, activityManager, state);
    else
      state.blocked = true;
    return false;
  }

  if (!state.active) {
    const TwoFingerSwipe::CompletedSwipe swipe = {2, state.startX, state.startY, centerX, centerY, 0};
    const auto direction = TwoFingerSwipe::directionFor(swipe, renderer.getScreenWidth(), renderer.getScreenHeight());
    const uint8_t action = actionForTwoFingerDirection(direction);
    if (!isLightSwipeAction(action) || !TwoFingerSwipe::fingersMovedTogether(start, current, direction)) {
      return false;
    }
    state.active = true;
    state.action = action;
    state.direction = static_cast<int>(direction);
    state.vertical = direction == TwoFingerSwipe::Direction::Up || direction == TwoFingerSwipe::Direction::Down;
    state.movementSign =
        direction == TwoFingerSwipe::Direction::Up || direction == TwoFingerSwipe::Direction::Left ? -1 : 1;
    const bool brightness = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                            action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS;
    if (brightness) TransferLightPulse::yieldToUser();  // slide from the user's level, not the pulse's
    state.initialOn = Frontlight.isOn();
    state.initialValue = brightness ? Frontlight.brightness() : Frontlight.warmth();
  }

  const int displacement = state.movementSign * (state.vertical ? centerY - state.startY : centerX - state.startX);
  const int axisSize = state.vertical ? renderer.getScreenHeight() : renderer.getScreenWidth();
  updateLiveLightSwipe(activity, activityManager, state, SwipeAdjustment::liveAmount(displacement, axisSize));
  return true;
}
#endif

bool applyTwoFingerSwipeAction(Activity& activity, MappedInputManager& mappedInput, const GfxRenderer& renderer,
                               ActivityManager& activityManager) {
  MappedInputManager::CompletedSwipe completed;
  if (!mappedInput.wasCompletedMultiTouchSwipe(completed)) return false;

  const TwoFingerSwipe::CompletedSwipe swipe = {completed.contactCount, completed.startX, completed.startY,
                                                completed.endX,         completed.endY,   completed.durationMs};
  const auto direction = TwoFingerSwipe::directionFor(swipe, renderer.getScreenWidth(), renderer.getScreenHeight());
  const uint8_t action = actionForTwoFingerDirection(direction);
  if (action == CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET) return false;
  const bool vertical = direction == TwoFingerSwipe::Direction::Up || direction == TwoFingerSwipe::Direction::Down;
  const int distance =
      vertical ? std::abs(completed.endY - completed.startY) : std::abs(completed.endX - completed.startX);
  const int axisSize = vertical ? renderer.getScreenHeight() : renderer.getScreenWidth();
  return applyConfiguredSwipeAction(activity, activityManager, action, SwipeAdjustment::edgeAmount(distance, axisSize));
}

#if CROSSDINK_APP_CAP_TOUCH
bool applyEdgeSlideAction(Activity& activity, MappedInputManager& mappedInput, ActivityManager& activityManager,
                          LiveLightSwipeState& state) {
  MappedInputManager::EdgeSlideProgress progress;
  if (!mappedInput.getEdgeSlideProgress(progress)) return false;
  uint8_t action = CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET;
  switch (progress.direction) {
    case MappedInputManager::EdgeSlide::LeftUp:
      action = SETTINGS.leftEdgeUp;
      break;
    case MappedInputManager::EdgeSlide::LeftDown:
      action = SETTINGS.leftEdgeDown;
      break;
    case MappedInputManager::EdgeSlide::RightUp:
      action = SETTINGS.rightEdgeUp;
      break;
    case MappedInputManager::EdgeSlide::RightDown:
      action = SETTINGS.rightEdgeDown;
      break;
    case MappedInputManager::EdgeSlide::None:
      break;
  }
  if (state.active) {
    // Track the finger in both directions: reversing through the touch-down
    // point keeps moving the value instead of holding at the starting value.
    const int amount =
        SwipeAdjustment::liveAmount(state.movementSign * progress.deltaY, mappedInput.getRenderer().getScreenHeight());
    updateLiveLightSwipe(activity, activityManager, state, amount);
    if (progress.finished) {
      mappedInput.suppressCurrentTouchContact();
      finishLiveLightSwipe(state, activityManager);
    }
    return true;
  }
  if (action == CrossPointSettings::TWO_FINGER_SWIPE_NOT_SET) return false;
  if (isLightSwipeAction(action)) {
    if (action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS && !Frontlight.isOn()) {
      // A dimming slide (whichever way the user set it) leaves an off light off at its level.
      if (progress.finished) mappedInput.suppressCurrentTouchContact();
      return true;
    }
    state = {};
    state.owner = &activity;
    state.active = true;
    state.action = action;
    state.direction = static_cast<int>(progress.direction);
    state.movementSign = progress.direction == MappedInputManager::EdgeSlide::LeftUp ||
                                 progress.direction == MappedInputManager::EdgeSlide::RightUp
                             ? -1
                             : 1;
    const bool brightness = action == CrossPointSettings::TWO_FINGER_SWIPE_INCREASE_BRIGHTNESS ||
                            action == CrossPointSettings::TWO_FINGER_SWIPE_DECREASE_BRIGHTNESS;
    if (brightness) TransferLightPulse::yieldToUser();  // slide from the user's level, not the pulse's
    state.initialOn = Frontlight.isOn();
    state.initialValue = brightness ? Frontlight.brightness() : Frontlight.warmth();
    updateLiveLightSwipe(activity, activityManager, state,
                         SwipeAdjustment::edgeAmount(progress.distance, mappedInput.getRenderer().getScreenHeight()));
    if (progress.finished) {
      mappedInput.suppressCurrentTouchContact();
      finishLiveLightSwipe(state, activityManager);
    }
    return true;
  }
  if (!progress.finished) return false;
  mappedInput.suppressCurrentTouchContact();
  return applyConfiguredSwipeAction(activity, activityManager, action);
}
#endif

bool applyTwoFingerRotation(Activity& activity, MappedInputManager& mappedInput) {
  if (!SETTINGS.twoFingerRotationEnabled) return false;
  MappedInputManager::CompletedRotation completed;
  if (!mappedInput.wasCompletedMultiTouchRotation(completed)) return false;
  // Rotate the content opposite the physical gesture so it feels like the
  // reader is turning the page under the user's fingers.
  return activity.handleTwoFingerRotation(completed.degrees < 0.0f);
}
}  // namespace

void ActivityManager::begin(const uint32_t renderTaskStackBytes) {
#if CROSSDINK_SCALABLE_FONTS
  ScalableFontAccess::configure(renderingMutex);
#endif
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender", renderTaskStackBytes,
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          TaskCores::kUi      // Keep long renders/cover decodes off CPU 0's idle watchdog
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
  LOG_DBG("ACT", "Render task started with %lu-byte stack", static_cast<unsigned long>(renderTaskStackBytes));
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  bool renderQueued = false;
  bool displayPmHeld = false;
  bool idlePanelOffArmed = false;
  uint32_t idlePanelOffMs = PANEL_OFF_POLL_MS;
  while (true) {
    if (!renderQueued) {
      const uint32_t notified =
          ulTaskNotifyTake(pdTRUE, idlePanelOffArmed ? pdMS_TO_TICKS(idlePanelOffMs) : portMAX_DELAY);
      if (notified == 0) {
        // No new frame within the idle delay (right after the draw on screens
        // that opted in): switch the booster off. The next refresh, or an
        // early wake on input, powers it back on.
        idlePanelOffArmed = false;
#ifndef SIMULATOR  // the simulator HAL has no panel power
        RenderLock offLock;
        if (display.isRefreshPending() || display.isRefreshBusy()) {
          // A deferred refresh is still driving the panel: never cut the
          // booster mid-waveform; poll again until it ends.
          idlePanelOffArmed = true;
          idlePanelOffMs = PANEL_OFF_POLL_MS;
        } else if (currentActivity && display.powerOffIdle()) {
          panelBoosterOff.store(true, std::memory_order_release);
          LOG_DBG("ACT", "Panel booster off after %s", idlePanelOffMs == PANEL_IDLE_OFF_MS ? "idle" : "draw");
        }
#endif
        continue;
      }
      if (notified == PANEL_WAKE_BIT) {
        // Input with no frame requested yet (finger down, button press):
        // power the booster on now, so the coming refresh skips its ~127 ms
        // power-on. Nothing drawn: switch it off again after the idle delay.
#ifndef SIMULATOR
        RenderLock wakeLock;
        if (display.powerOnIdle()) LOG_DBG("ACT", "Panel booster on early");
#endif
        idlePanelOffArmed = true;
        idlePanelOffMs = PANEL_IDLE_OFF_MS;
        continue;
      }
    }
    renderQueued = false;
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    TouchRegistry::getInstance().setEnabled(mappedInput.hasTouch());
    TouchRegistry::getInstance().beginFrame();
    bool deferredRender = false;
    bool batchInput = false;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      // Apply Night Mode to each activity's normal-polarity frame. SleepActivity
      // preserves it only for Quick Resume and clears it for other sleep screens.
      display.setInverted(SETTINGS.screenInverted != 0);
      taskENTER_CRITICAL(&renderStateMux);
      const bool waiterPending = waitingTaskHandle != nullptr;
      taskEXIT_CRITICAL(&renderStateMux);
      // A waiter expects the frame to be visible when it wakes, so its render
      // keeps the blocking refresh.
      // Always false on boards without PSRAM, where deferred refresh is off.
      // cppcheck-suppress knownConditionTrueFalse
      deferredRender = !waiterPending && allowsDeferredRefresh(*currentActivity);
      batchInput = currentActivity->batchesInputDuringRefresh();
      renderer.setDeferFastRefresh(deferredRender);
      PerfLog::noteRenderStart(currentActivity->name.c_str());
      // Interactive screens keep the booster on so input never waits on PON.
      idlePanelOffArmed = currentActivity->powerOffPanelWhenIdle();
      idlePanelOffMs = PANEL_OFF_POLL_MS;
      panelBoosterOff.store(false, std::memory_order_release);  // this frame's refresh powers it on
      BaseTheme::beginFrameStatus();
      if (currentActivity.get() != listSelectionOwner) {
        listSelectionOwner = currentActivity.get();
        ListSelection::tapRowShown = false;  // a popup of the previous screen
      }
      ListSelection::revealed = currentActivity->listSelectionRevealed;
      ListSelection::hidThisFrame = false;
      currentActivity->render(std::move(lock));
      currentActivityPainted.store(true, std::memory_order_release);
      ListSelection::hidOnScreen = ListSelection::hidThisFrame;
      PerfLog::noteRenderEnd();
      renderer.setDeferFastRefresh(false);
      restoredActivityNeedsRender = false;
    }
    TouchRegistry::getInstance().publish();
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&renderStateMux);
    waiter = waitingTaskHandle;
    waitingTaskHandle = nullptr;
    taskEXIT_CRITICAL(&renderStateMux);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }

    // A deferred menu refresh is still running on the panel. Release the render
    // lock so input and screen changes proceed meanwhile, and finish the refresh
    // once the waveform ends. A new render request goes first: its own display
    // call finishes this refresh before sending the next frame. Screens that
    // batch input keep collecting requests until the waveform ends, so the next
    // frame shows every keystroke made meanwhile instead of only the first.
    if (deferredRender && renderer.isRefreshPending()) {
      if (!displayPmHeld) {
        powerManager.beginDisplayRefreshHold();  // no light sleep mid-waveform
        displayPmHeld = true;
      }
      lock.unlock();
      while (true) {
        // An early-wake bit alone is no frame: the booster is on mid-refresh.
        if ((ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DEFERRED_REFRESH_POLL_MS)) & ~PANEL_WAKE_BIT) != 0) {
          renderQueued = true;
          if (!batchInput) break;
        }
        RenderLock finishLock;
        // The refresh can complete while this task waits for the notification.
        // cppcheck-suppress knownConditionTrueFalse
        if (!renderer.isRefreshPending()) break;
        if (!renderer.isRefreshBusy()) {
          renderer.waitRefreshComplete();
          // Already over, so the busy-wait hook that marks ink never fired.
          PerfLog::noteInk();
          break;
        }
      }
    }
    // Other renders finish any refresh left pending here with their own display
    // calls, and readers manage the refreshes they start themselves.
    if (displayPmHeld && (!deferredRender || !renderer.isRefreshPending())) {
      powerManager.endDisplayRefreshHold();
      displayPmHeld = false;
    }
    // Renders leave work for the loop (queued page turns, toasts, alerts,
    // prerender timers): one pass now instead of at the next idle tick.
    InputTask::wakeLoop();
  }
}

bool ActivityManager::allowsDeferredRefresh(const Activity& activity) {
#if defined(BOARD_HAS_PSRAM)
  // Readers run their own overlapped refresh and grayscale flows. Sleep and
  // Boot must reach the panel before the device powers down or moves on, and
  // the image viewer follows its refresh with grayscale plane uploads.
  return !activity.isReaderActivity() && activity.name != "Sleep" && activity.name != "Boot" &&
         activity.name != "BmpViewer";
#else
  // The 48 KB baseline copy the deferred refresh needs is too much internal RAM
  // for boards without PSRAM.
  (void)activity;
  return false;
#endif
}

void ActivityManager::loop() {
  if (currentActivity && currentActivity->requiresExclusiveStorageLoop()) {
    currentActivity->loop();
    // USB Drive normally restarts the device rather than replacing itself. The
    // pending-action fallthrough keeps the simulator's stub lifecycle usable.
    if (pendingAction == PendingAction::None) {
      if (requestedUpdate.exchange(false) && renderTaskHandle) {
        xTaskNotify(renderTaskHandle, 1, eIncrement);
      }
      return;
    }
  }

#if CROSSDINK_APP_CAP_TOUCH
  if (edgeLightSwipe.active && edgeLightSwipe.owner != currentActivity.get())
    finishLiveLightSwipe(edgeLightSwipe, *this);
  if (twoFingerLightSwipe.tracking && twoFingerLightSwipe.owner != currentActivity.get())
    finishLiveLightSwipe(twoFingerLightSwipe, *this);
#endif

  if (currentActivity) {
    mappedInput.setPowerAsConfirmInReaderMode(currentActivity->allowPowerAsConfirmInReaderMode());

#if CROSSDINK_APP_CAP_TOUCH
    // A touch list opens with no row selected. The first Up/Down/Confirm press
    // only shows the selection (no move, no activation); its release is eaten.
    if (ListSelection::hidOnScreen && !currentActivity->listSelectionRevealed) {
      using Button = MappedInputManager::Button;
      const bool up = mappedInput.wasPressed(Button::Up);
      const bool down = mappedInput.wasPressed(Button::Down);
      const bool confirm = mappedInput.wasPressed(Button::Confirm);
      if (up || down || confirm) {
        currentActivity->listSelectionRevealed = true;
        ListSelection::hidOnScreen = false;
        if (up) mappedInput.suppressNextSideRelease(Button::Up);
        if (down) mappedInput.suppressNextSideRelease(Button::Down);
        if (confirm) mappedInput.suppressNextConfirmRelease();
        requestUpdate();
        return;
      }
    }
#endif

    if (currentActivity->blocksGlobalInput()) {
#if CROSSDINK_APP_CAP_TOUCH
      if (edgeLightSwipe.active) finishLiveLightSwipe(edgeLightSwipe, *this);
      if (twoFingerLightSwipe.tracking) finishLiveLightSwipe(twoFingerLightSwipe, *this);
#endif
      mappedInput.resetEdgeSlide();
      currentActivity->loop();
    } else {
#if CROSSDINK_APP_CAP_TOUCH
      if (currentActivity->name != "FrontlightPanel" &&
          applyLiveTwoFingerLightSwipe(*currentActivity, mappedInput, renderer, *this, twoFingerLightSwipe,
                                       edgeLightSwipe)) {
        return;
      }
#endif
      // Completed two-finger gestures are recognized before normal one-finger
      // activity gestures. A rotation has priority over translation in the SDK,
      // so a contact sequence can trigger at most one action here.
      if (applyTwoFingerRotation(*currentActivity, mappedInput)) {
        return;
      }

      // The frontlight panel owns its own sliders.
      if (currentActivity->name != "FrontlightPanel" &&
          applyTwoFingerSwipeAction(*currentActivity, mappedInput, renderer, *this)) {
        return;
      }

#if CROSSDINK_APP_CAP_TOUCH
      if (applyEdgeSlideAction(*currentActivity, mappedInput, *this, edgeLightSwipe)) {
        return;
      }
#endif

      // Frontlight quick panel: top-edge down-swipe on home-key boards, except
      // that the open EPUB reader exposes the same action across the whole page.
      // Pushed, so it returns to whatever was underneath — including mid-book.
      const bool lightPanelGesture = currentActivity->usesFullScreenReaderVerticalSwipes()
                                         ? mappedInput.wasReaderLightPanelGesture()
                                         : mappedInput.wasLightPanelGesture();
      if (supportsFrontlightDrawer(mappedInput.hasTouchHardware(), Frontlight.present(),
                                   hasStickyReaderDetailsPanel()) &&
          currentActivity->name != "FrontlightPanel" && currentActivity->allowFrontlightPanelGesture() &&
          lightPanelGesture) {
        openFrontlightPanel(*currentActivity, renderer, mappedInput);
        return;
      }
      // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
      if (!handleGlobalHomeGesture()) {
        currentActivity->loop();
      }
    }
  } else {
    mappedInput.setPowerAsConfirmInReaderMode(false);
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      const bool closedFrontlightPanel = currentActivity->name == "FrontlightPanel";
      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      settingsFlushPending = true;
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        restoredActivityNeedsRender = true;
        currentActivityPainted.store(false, std::memory_order_release);

        if (closedFrontlightPanel) currentActivity->onFrontlightPanelClosed();

        // Handle result if necessary
        if (currentActivity->resultHandler) {
          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Queue an update to ensure the popped activity gets re-rendered. A
        // partial-screen overlay first restores the full-screen activity below
        // it now that the result handler has finished reconciling settings.
        if (pendingAction == PendingAction::None) {
          lock.unlock();
          if (currentActivity->requiresFreshBackdrop()) restoreBackdropBehindCurrentOverlay();
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      if (pendingAction == PendingAction::Push && pendingActivity->requiresFreshBackdrop()) {
        if (restoredActivityNeedsRender.load()) {
          const RequestUpdateResult redraw = requestUpdateAndWait();
          if (redraw != RequestUpdateResult::Rendered) {
            LOG_ERR("ACT", "Could not restore source backdrop before opening %s", pendingActivity->name.c_str());
          }
        }
        // A queued render may already have refreshed the source and cleared
        // restoredActivityNeedsRender. Preserve the overlay's paused timing
        // state either way before it becomes current.
        if (currentActivity) currentActivity->onBackdropRenderedForOverlay();
      } else if (pendingAction == PendingAction::Push && pendingActivity->drawsOverSourceFrame() &&
                 mappedInput.wasTapOrHeld()) {
        // The tapped row is selected but not yet on screen: show it before the
        // popup covers the frame, not after the popup closes.
        ListSelection::tapRowShown = true;
        if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
          LOG_ERR("ACT", "Could not show tapped row before opening %s", pendingActivity->name.c_str());
        }
        ListSelection::tapRowShown = false;
      }
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        settingsFlushPending = true;
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // Move current activity to stack
        currentActivity->onCovered();
        stackActivities.push_back(std::move(currentActivity));
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);
      currentActivityPainted.store(false, std::memory_order_release);

      lock.unlock();  // onEnter may acquire its own lock
      // Only Home waits for its first frame; anything else (USB Drive included)
      // must find the reader's exit writes on SD before it starts.
      if (!currentActivity->isHomeActivity()) ReaderExitSave::flush();
      // USB Drive owns the card from onEnter() and skips the deferred flush below.
      if (settingsFlushPending && currentActivity->requiresExclusiveStorageLoop()) {
        settingsFlushPending = false;
        flushSettingsStores();
      }
#if CROSSDINK_GOODIES
      // The Goodies Wi-Fi remote's join task must be done before this screen takes the radio.
      if (currentActivity->usesWifi()) goodies_remote::waitForJoin();
#endif
      if (currentActivity->usesWifi()) {
        kosync_auto::yieldRadio();
        wifi_background_join::wait();  // likewise the OPDS list's join or teardown task
      }
      currentActivity->onEnter();

      // cppcheck-suppress knownConditionTrueFalse ; onEnter() above may queue another navigation
      if (pendingAction == PendingAction::None && pendingReaderMenuAction >= 0 &&
          currentActivity->isEpubReaderActivity()) {
        const uint8_t action = static_cast<uint8_t>(pendingReaderMenuAction);
        pendingReaderMenuAction = -1;
        currentActivity->handleExternalReaderMenuAction(action);
      }

      // cppcheck-suppress knownConditionTrueFalse ; onEnter() above may queue another navigation
      if (pendingAction == PendingAction::None && APP_STATE.pendingOverlayResume.valid()) {
        const PendingOverlayResume resume = APP_STATE.pendingOverlayResume;
        if (resume.returnHomeAfterReaderFlow && currentActivity->isEpubReaderActivity() &&
            (resume.bookPath.empty() || resume.bookPath == currentActivity->getCurrentBookPath())) {
          PendingOverlayResume homeResume = resume;
          homeResume.returnHomeAfterReaderFlow = false;
          APP_STATE.setPendingOverlayResume(std::move(homeResume));
          goHome();
          continue;
        }
        const bool readerReady = resume.origin == PendingOverlayOrigin::Reader &&
                                 currentActivity->isEpubReaderActivity() &&
                                 (resume.bookPath.empty() || resume.bookPath == currentActivity->getCurrentBookPath());
        const bool homeReady = resume.origin == PendingOverlayOrigin::Home && currentActivity->isHomeActivity();
        if (readerReady || homeReady) {
          if (resume.overlay == PendingOverlayType::ReaderDrawer && currentActivity->restorePendingOverlay(resume)) {
            PendingOverlayResume consumed;
            APP_STATE.consumePendingOverlayResume(consumed);
          } else if (resume.overlay == PendingOverlayType::FrontlightDrawer &&
                     supportsFrontlightDrawer(mappedInput.hasTouchHardware(), Frontlight.present(),
                                              hasStickyReaderDetailsPanel())) {
            FrontlightDrawerState restoredState;
            restoredState.selectedAction = static_cast<int8_t>(resume.selectedIndex);
            if (openFrontlightPanel(*currentActivity, renderer, mappedInput, &restoredState)) {
              PendingOverlayResume consumed;
              APP_STATE.consumePendingOverlayResume(consumed);
            }
          }
        }
      }

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  // The new screen's first frame is refreshing on the panel: the reader's exit
  // writes (Home) and deferred settings run in that wait instead of before it drew.
  if (!currentActivity || currentActivityPainted.load(std::memory_order_acquire)) {
    ReaderExitSave::flush();
    // Settings changed on a screen or panel are written once it has closed,
    // while the screen below refreshes.
    if (settingsFlushPending && !(currentActivity && currentActivity->holdsSettingsFlush())) {
      settingsFlushPending = false;
      flushSettingsStores();
    }
  }

  if (APP_STATE.hasPendingAlert.load(std::memory_order_acquire) && pendingAction == PendingAction::None) {
    APP_STATE.hasPendingAlert.store(false, std::memory_order_relaxed);
    pushActivity(std::make_unique<AlertActivity>(renderer, mappedInput));
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

bool ActivityManager::restoreBackdropBehindCurrentOverlay() {
  if (!currentActivity || !currentActivity->requiresFreshBackdrop() || stackActivities.empty()) return false;

  std::unique_ptr<Activity> overlay;
  {
    RenderLock lock;
    overlay = std::move(currentActivity);
    currentActivity = std::move(stackActivities.back());
    stackActivities.pop_back();
  }

  const RequestUpdateResult redraw = requestUpdateAndWait();
  if (redraw == RequestUpdateResult::Rendered && currentActivity) {
    currentActivity->onBackdropRenderedForOverlay();
  } else {
    LOG_ERR("ACT", "Could not restore backdrop behind %s", overlay->name.c_str());
  }

  {
    RenderLock lock;
    if (currentActivity) currentActivity->onCovered();
    stackActivities.push_back(std::move(currentActivity));
    currentActivity = std::move(overlay);
  }
  return redraw == RequestUpdateResult::Rendered;
}

bool ActivityManager::handleGlobalHomeGesture() {
  if (!currentActivity || pendingAction != PendingAction::None || currentActivity->isHomeActivity() ||
      !currentActivity->allowGlobalHomeGesture() || (!mappedInput.hasTouch() && !mappedInput.hasHomeKey())) {
    return false;
  }

  const bool homeGesture = currentActivity->usesFullScreenReaderVerticalSwipes()
                               ? mappedInput.wasReaderHomeGesture()
                               : (currentActivity->allowGlobalHomeSwipeGesture() || mappedInput.hasHomeKey()) &&
                                     mappedInput.wasHomeGesture();
  if (!homeGesture) {
    return false;
  }

  // Touch-only devices use an edge swipe as a Home shortcut. Keep that
  // shortcut separate from the X4 Pro's physical Back/Home key.
  if (!mappedInput.hasHomeKey()) {
    if (!currentActivity->handleHomeGesture()) goHome();
    return true;
  }

  return handleHomeButtonBackOrHome();
}

bool ActivityManager::handleHomeButtonBackOrHome() {
  if (!currentActivity || pendingAction != PendingAction::None || currentActivity->isHomeActivity()) {
    return false;
  }

  if (currentActivity->handleHomeGesture()) {
    return true;
  }

  if (!stackActivities.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    currentActivity->setResult(std::move(result));
    popActivity();
    return true;
  }

  goHome();
  return true;
}

bool ActivityManager::openReaderMenuFromShortcut() {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->openReaderSettingsMenu();
}

bool ActivityManager::handleShortcutAction(const uint8_t action) {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->handleShortcutAction(action);
}

bool ActivityManager::handleQuickLockUnlock(const QuickLockTrigger trigger) {
  return currentActivity && pendingAction == PendingAction::None && currentActivity->handleQuickLockUnlock(trigger);
}

void ActivityManager::notifyInputLockChanged(const bool locked) {
  if (currentActivity) currentActivity->onInputLockChanged(locked);
  for (const auto& activity : stackActivities) {
    activity->onInputLockChanged(locked);
  }
}

void ActivityManager::cancelOptionalRenderWork(const char* reason) {
  if (currentActivity) currentActivity->cancelOptionalRenderWork(reason);
}

void ActivityManager::notifyUserInput() {
  if (currentActivity) currentActivity->onUserInput();
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    TouchRegistry::getInstance().clear();
    currentActivity->onExit();
    currentActivity.reset();
    finishNetworkExit();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    TouchRegistry::getInstance().clear();
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    TouchRegistry::getInstance().clear();
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::goToFileTransfer(std::string returnBookPath) {
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput, std::move(returnBookPath)));
}

bool ActivityManager::goToNearbyBookSend(std::string path, const bool returnToReader) {
  auto activity = makeUniqueNoThrow<NearbyBookTransferActivity>(
      renderer, mappedInput, NearbyBookTransferActivity::Mode::Send, std::move(path), returnToReader);
  if (!activity) {
    LOG_ERR("ACT", "OOM: nearby file sender");
    return false;
  }
  replaceActivity(std::move(activity));
  return true;
}

void ActivityManager::goToNearbyBookReceive() {
  auto activity =
      makeUniqueNoThrow<NearbyBookTransferActivity>(renderer, mappedInput, NearbyBookTransferActivity::Mode::Receive);
  if (!activity) {
    LOG_ERR("ACT", "OOM: nearby file receiver");
    return;
  }
  replaceActivity(std::move(activity));
}

// cppcheck-suppress functionStatic ; per-instance navigation API, delegates to the ActivityManager singleton
void ActivityManager::goToCalibreWireless(const std::string& returnBookPath) {
  restartToFileTransfer(NetworkMode::CONNECT_CALIBRE, returnBookPath);
}

// cppcheck-suppress functionStatic ; per-instance navigation API, delegates to the ActivityManager singleton
void ActivityManager::goToJoinNetworkFileTransfer(const std::string& returnBookPath) {
  restartToFileTransfer(NetworkMode::JOIN_NETWORK, returnBookPath);
}

// cppcheck-suppress functionStatic ; per-instance navigation API, delegates to the ActivityManager singleton
void ActivityManager::goToHotspotFileTransfer(const std::string& returnBookPath) {
  restartToFileTransfer(NetworkMode::CREATE_HOTSPOT, returnBookPath);
}

void ActivityManager::goToUsbDrive() {
#if CROSSDINK_APP_CAP_USB_DRIVE
  auto activity = makeUniqueNoThrow<UsbDriveActivity>(renderer, mappedInput);
  if (!activity) {
    LOG_ERR("ACT", "OOM: USB Drive activity");
    return;
  }
  replaceActivity(std::move(activity));
#else
  LOG_ERR("ACT", "USB Drive requested in a build without USB Drive capability");
#endif
}

bool ActivityManager::resumeFileTransferFromNetworkBoot(const uint32_t payload) {
  const uint32_t rawMode = payload & FILE_TRANSFER_MODE_MASK;
  if (rawMode > static_cast<uint32_t>(NetworkMode::CREATE_HOTSPOT)) {
    LOG_ERR("ACT", "Invalid file transfer network boot mode: %lu", static_cast<unsigned long>(rawMode));
    return false;
  }

  std::string returnBookPath;
  if ((payload & FILE_TRANSFER_RETURN_TO_READER) != 0) {
    if (!APP_STATE.openEpubPath.empty() && Storage.exists(APP_STATE.openEpubPath.c_str())) {
      returnBookPath = APP_STATE.openEpubPath;
    } else {
      LOG_ERR("ACT", "File transfer cannot return to missing reader path: %s", APP_STATE.openEpubPath.c_str());
    }
  }

  // The activity must outlive this boot function, so allocate its small control object on the heap; web buffers
  // remain owned and released by the activity lifecycle.
  auto activity = makeUniqueNoThrow<CrossPointWebServerActivity>(
      renderer, mappedInput, static_cast<NetworkMode>(rawMode), std::move(returnBookPath), true);
  if (!activity) {
    LOG_ERR("ACT", "OOM: file transfer after minimal boot (free=%" PRIu32 " maxAlloc=%" PRIu32 ")", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }

  replaceActivity(std::move(activity));
  return true;
}

void ActivityManager::goToNearbyStatsSync() {
  if (!SETTINGS.shouldTrackReadingStats()) return;
  replaceActivity(std::make_unique<NearbyStatsSyncActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings(const bool dismissOnUpSwipe) {
  preferredHomeBookPath.clear();
  returningHomeThroughSettings = false;
  if (currentActivity && currentActivity->isHomeActivity()) {
    preferredHomeBookPath = currentActivity->getCurrentBookPath();
    returningHomeThroughSettings = true;
  } else if (!stackActivities.empty() && stackActivities.back()->isHomeActivity()) {
    preferredHomeBookPath = stackActivities.back()->getCurrentBookPath();
    returningHomeThroughSettings = true;
  }
  replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput, dismissOnUpSwipe));
}

void ActivityManager::goToFileBrowser(std::string path) {
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

bool ActivityManager::goToLibrary() {
  auto library = makeUniqueNoThrow<LibraryActivity>(renderer, mappedInput);
  if (!library) {
    LOG_ERR("ACT", "Cannot allocate Library activity");
    return false;
  }
  replaceActivity(std::move(library));
  return true;
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    goToOpdsServer(0);
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

bool ActivityManager::goToOpdsServer(const uint32_t serverIndex, const bool networkBootReady) {
#ifndef SIMULATOR
  if (!networkBootReady) {
    silentRestartToNetwork(NetworkBootTarget::OPDS, serverIndex);
    return true;
  }
#else
  // The desktop build has no fragmented WiFi heap to clear; keep the preview in-process.
  (void)networkBootReady;
#endif

  OPDS_STORE.loadFromFile();
  const auto* storedServer = OPDS_STORE.getServer(serverIndex);
  if (!storedServer) {
    LOG_ERR("ACT", "OPDS network boot server index out of range: %lu", static_cast<unsigned long>(serverIndex));
    return false;
  }

  OpdsServer server = *storedServer;
  OPDS_STORE.release();
  auto browser = makeUniqueNoThrow<OpdsBookBrowserActivity>(renderer, mappedInput, std::move(server));
  if (!browser) {
    LOG_ERR("ACT", "OOM: OPDS browser after minimal boot (free=%" PRIu32 " maxAlloc=%" PRIu32 ")", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  replaceActivity(std::move(browser));
  return true;
}

void ActivityManager::goToReader(std::string path, const bool suppressBackRelease, const bool allowFastInitialRefresh,
                                 const bool cleanImageBaseOnEntry) {
  // OPDS credentials are unrelated to local reading and may contain several
  // heap-backed strings. Home reloads them lazily when it becomes active.
  OPDS_STORE.release();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), suppressBackRelease,
                                                   allowFastInitialRefresh, cleanImageBaseOnEntry));
}

void ActivityManager::goToReaderAndRunMenuAction(std::string path, const uint8_t action) {
  pendingReaderMenuAction = action;
  goToReader(std::move(path));
}

void ActivityManager::goToSleep(bool fromTimeout) {
  // The sleep screen waits on the render lock; don't make it wait on AA.
  cancelOptionalRenderWork(fromTimeout ? "auto-sleep" : "sleep");
  const bool canSnapshotOverlay = currentActivity && currentActivity->canSnapshotForSleepOverlay();
  const GfxRenderer::Orientation sleepPopupOrientation = renderer.getOrientation();
  std::string currentBookPath = getCurrentBookPath();
  const bool renderBeforeExit = currentActivity && SleepActivity::rendersBeforeExit(currentBookPath, fromTimeout);
  auto sleepActivity = std::make_unique<SleepActivity>(renderer, mappedInput, canSnapshotOverlay,
                                                       std::move(currentBookPath), fromTimeout, sleepPopupOrientation);
  if (renderBeforeExit) {
    // Draw the sleep screen first, then let the outgoing activities save their
    // progress and stats. Holding the render lock throughout keeps the render
    // task from repainting the outgoing activity over the sleep screen.
    RenderLock lock;
    TouchRegistry::getInstance().clear();
    sleepActivity->onEnter();
    exitActivity(lock);
    while (!stackActivities.empty()) {
      stackActivities.back()->onExit();
      stackActivities.pop_back();
    }
    pendingActivity.reset();
    pendingAction = PendingAction::None;
    currentActivity = std::move(sleepActivity);
    return;
  }
  replaceActivity(std::move(sleepActivity));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::exitAllActivities() {
  RenderLock lock;
  exitActivity(lock);
  while (!stackActivities.empty()) {
    stackActivities.back()->onExit();
    stackActivities.pop_back();
  }
  pendingActivity.reset();
  pendingAction = PendingAction::None;
}

void ActivityManager::goToBoot() { replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput)); }

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem, const HalDisplay::RefreshMode initialRefreshMode) {
  std::string initialBookPath;
  if (returningHomeThroughSettings) {
    initialBookPath = std::move(preferredHomeBookPath);
  }
  preferredHomeBookPath.clear();
  returningHomeThroughSettings = false;

  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "Library") {
      initialMenuItem = HomeMenuItem::LIBRARY;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "NearbyStatsSync") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem, initialRefreshMode,
                                                 std::move(initialBookPath)));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  TouchRegistry::getInstance().clear();
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  TouchRegistry::getInstance().clear();
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::requiresExclusiveStorageLoop() const {
  return currentActivity && currentActivity->requiresExclusiveStorageLoop();
}

bool ActivityManager::blocksGlobalInput() const { return currentActivity && currentActivity->blocksGlobalInput(); }

bool ActivityManager::isRenderIdle() const {
  if (requestedUpdate.load() || RenderLock::peek() || renderer.isRefreshPending()) return false;
  // Notified-but-not-yet-running shows as Ready; waiting for work is Blocked.
  return renderTaskHandle == nullptr || eTaskGetState(renderTaskHandle) == eBlocked;
}

bool ActivityManager::isHomeActivity() const { return currentActivity && currentActivity->name == "Home"; }

bool ActivityManager::isReaderActivity() const {
  if (currentActivity && currentActivity->isReaderActivity()) {
    return true;
  }

  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity && activity->isReaderActivity(); });
}

bool ActivityManager::openReaderSettingsForTouchscreenEscapeHatch() {
  if (!mappedInput.hasTouchHardware() || !SETTINGS.disableReaderTouchscreen || !currentActivity ||
      !currentActivity->isReaderActivity() || pendingAction != PendingAction::None) {
    return false;
  }

  if (!currentActivity->openReaderSettingsMenu()) {
    // TXT has no reader menu, so keep a reliable global Settings fallback.
    goToSettings();
  }
  return true;
}

bool ActivityManager::anyActivityUsesWifi() const {
  const auto uses = [](const auto& activity) { return activity && activity->usesWifi(); };
  return uses(currentActivity) || uses(pendingActivity) ||
         std::any_of(stackActivities.begin(), stackActivities.end(), uses);
}

bool ActivityManager::wifiActivitiesShareRemote() const {
  const auto blocks = [](const auto& activity) {
    return activity && activity->usesWifi() && !activity->sharesWifiWithRemote();
  };
  return !blocks(currentActivity) && !blocks(pendingActivity) &&
         std::none_of(stackActivities.begin(), stackActivities.end(), blocks);
}

bool ActivityManager::hasActivityNamed(const char* activityName) const {
  const auto matches = [activityName](const auto& activity) { return activity && activity->name == activityName; };
  if (matches(currentActivity) || matches(pendingActivity)) {
    return true;
  }

  return std::any_of(stackActivities.begin(), stackActivities.end(), matches);
}

#ifdef SIMULATOR
bool ActivityManager::isCurrentActivityNamed(const char* activityName) const {
  return currentActivity && currentActivity->name == activityName;
}
#endif

bool ActivityManager::canSnapshotForSleepOverlay() const {
  return currentActivity && currentActivity->canSnapshotForSleepOverlay();
}

bool ActivityManager::requestManualReaderRefresh() {
  cancelOptionalRenderWork("refresh");
  RenderLock lock;
  if (!currentActivity || !currentActivity->isReaderActivity() || !currentActivity->prepareManualRefresh()) {
    return false;
  }

  lock.unlock();
  requestUpdate(true);
  return true;
}

bool ActivityManager::handleShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  return currentActivity && (currentActivity->isReaderActivity() || currentActivity->isHomeActivity()) &&
         currentActivity->handleShortcutAction(action);
}

Activity* ActivityManager::findEpubReader() const {
  if (currentActivity && currentActivity->isEpubReaderActivity()) return currentActivity.get();
  const auto reader = std::find_if(stackActivities.rbegin(), stackActivities.rend(),
                                   [](const auto& activity) { return activity && activity->isEpubReaderActivity(); });
  return reader != stackActivities.rend() ? reader->get() : nullptr;
}

void ActivityManager::persistGlobalSettings() {
  // A modal may be current while its EPUB reader, which owns the per-book
  // override, remains on the stack. Let that reader protect the global write.
  if (auto* reader = findEpubReader()) {
    reader->persistGlobalSettings();
  } else if (currentActivity) {
    currentActivity->persistGlobalSettings();
  } else {
    SETTINGS.saveToFile();
  }
  BatteryLog::lightChanged();  // every user light change (toggle, slide release, panel) saves here
}

bool ActivityManager::beginGlobalSettingsEdit() {
  auto* reader = findEpubReader();
  return reader && reader->onFrontlightGlobalSettingsOpened();
}

void ActivityManager::notifyExternalFrontlightChange() {
  if (currentActivity) currentActivity->onExternalFrontlightChange();
}

void ActivityManager::endGlobalSettingsEdit() {
  if (auto* reader = findEpubReader()) reader->onFrontlightGlobalSettingsClosed();
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

bool ActivityManager::allowsRadioIdleSleep() const {
  return currentActivity && currentActivity->allowsRadioIdleSleep();
}

std::string ActivityManager::getCurrentBookPath() const {
  if (currentActivity) {
    const std::string path = currentActivity->getCurrentBookPath();
    if (!path.empty()) {
      return path;
    }
  }

  for (auto it = stackActivities.rbegin(); it != stackActivities.rend(); ++it) {
    if (*it) {
      const std::string path = (*it)->getCurrentBookPath();
      if (!path.empty()) {
        return path;
      }
    }
  }

  return {};
}

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    const ScreenshotInfo info = currentActivity->getScreenshotInfo();
    if (info.readerType != ScreenshotInfo::ReaderType::None) {
      return info;
    }
  }

  // Reader overlays such as the dictionary are pushed above the book activity.
  // Keep their visible framebuffer, but inherit the book's screenshot filename.
  for (auto it = stackActivities.rbegin(); it != stackActivities.rend(); ++it) {
    if (*it) {
      const ScreenshotInfo info = (*it)->getScreenshotInfo();
      if (info.readerType != ScreenshotInfo::ReaderType::None) {
        return info;
      }
    }
  }

  return {};
}

void ActivityManager::wakePanelEarly() {
#ifndef SIMULATOR
  if (renderTaskHandle && panelBoosterOff.exchange(false, std::memory_order_acq_rel)) {
    xTaskNotify(renderTaskHandle, PANEL_WAKE_BIT, eSetBits);
  }
#endif
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
    InputTask::wakeLoop();  // from another task: the loop may be in an idle wait
  }
}
RequestUpdateResult ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return RequestUpdateResult::Rejected;
  }

  // Atomic section to perform checks
  taskENTER_CRITICAL(&renderStateMux);
  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool alreadyWaiting = (waitingTaskHandle != nullptr);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);
  if (!alreadyWaiting && !isRenderTask && !holdingRenderLock) {
    waitingTaskHandle = currTaskHandler;
  }
  taskEXIT_CRITICAL(&renderStateMux);

  if (isRenderTask) {
    LOG_ERR("ACT", "requestUpdateAndWait() called from render task; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  if (alreadyWaiting) {
    LOG_ERR("ACT", "requestUpdateAndWait() called while another task is waiting; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  // Cannot call while holding RenderLock or it will cause a deadlock
  if (holdingRenderLock) {
    LOG_ERR("ACT", "requestUpdateAndWait() called while holding RenderLock; rejecting sync update");
    return RequestUpdateResult::Rejected;
  }

  // This frame draws everything requested so far: drop a pending deferred
  // request so the next loop pass doesn't draw the same frame again.
  requestedUpdate = false;
  xTaskNotify(renderTaskHandle, 1, eIncrement);
  while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(RenderLock::WAIT_TICK_MS)) == 0) {
    if (RenderLock::waitTick) RenderLock::waitTick();
  }
  return RequestUpdateResult::Rendered;
}

// RenderLock

void (*RenderLock::waitTick)() = nullptr;

RenderLock::RenderLock(const Mode mode) {
  const TickType_t slice = mode == Mode::Try ? 0 : pdMS_TO_TICKS(WAIT_TICK_MS);
  while (!(isLocked = (xSemaphoreTake(activityManager.renderingMutex, slice) == pdTRUE)) && mode != Mode::Try) {
    if (waitTick) waitTick();
  }
}

RenderLock::RenderLock([[maybe_unused]] Activity&, const Mode mode) : RenderLock(mode) {}

RenderLock::RenderLock(const unsigned long timeoutMs) {
  isLocked = xSemaphoreTake(activityManager.renderingMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 * Checks if renderingMutex is held by any task, including the calling task.
 *
 * @return true if renderingMutex has an owner (any task), false otherwise.
 *
 * @note Must not be called from ISR context — xSemaphoreGetMutexHolder is not ISR-safe.
 */
bool RenderLock::peek() { return xSemaphoreGetMutexHolder(activityManager.renderingMutex) != nullptr; }

bool RenderLock::heldByCaller() {
  return xSemaphoreGetMutexHolder(activityManager.renderingMutex) == xTaskGetCurrentTaskHandle();
}

const char* ActivityManager::currentActivityName() const {
  return currentActivity ? currentActivity->name.c_str() : "";
}

bool ActivityManager::injectText(const char* utf8) { return currentActivity && currentActivity->injectText(utf8); }
