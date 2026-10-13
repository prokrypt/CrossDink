#include "BookActions.h"

#include <Arduino.h>
#include <Epub.h>
#include <Epub/EpubRenderMode.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <Xtc.h>

#include <cstdio>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "activities/ActivityManager.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsActivity.h"
#include "activities/reader/BookStatsTracking.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/OptionPopupActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"

namespace BookActions {
namespace {

bool hasReadingStats(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

// Main has no non-blocking toast yet: hold the toast 1 s, as the hosts did before.
void showToast(const GfxRenderer& renderer, const char* msg) {
  drawToast(renderer, msg);
  delay(1000);
}

// Ask to confirm, run, toast on success, then refresh the host.
void confirmThenRun(const Host& host, const StrId label, const std::string& fullPath, const std::string& title,
                    bool (*run)(const std::string&), const StrId doneMessage) {
  host.open(makeUniqueNoThrow<ConfirmationActivity>(host.renderer, host.mappedInput, confirmationHeading(label), title),
            [host, fullPath, run, doneMessage](const ActivityResult& confirmation) {
              if (!confirmation.isCancelled) {
                if (run(fullPath)) {
                  showToast(host.renderer, I18N.get(doneMessage));
                } else {
                  LOG_ERR("BookActions", "Book action failed for: %s", fullPath.c_str());
                }
              }
              host.refresh();
            });
}

std::string bookStatsCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crossdink").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crossdink").getCachePath();
  }
  return "";
}

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(includeRemoveFromRecents ? 9 : 8);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::EpubRenderMode, StrId::STR_EPUB_RENDER_MODE});
    items.push_back({FileBrowserAction::ResetReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS});
  }
  if (hasReadingStats(fullPath)) {
    if (SETTINGS.shouldTrackReadingStats()) {
      const bool bookEnabled = isBookStatsTrackingEnabled(fullPath);
      items.push_back({FileBrowserAction::ToggleBookStatsTracking, StrId::STR_TRACK_READING_STATS,
                       bookEnabled ? StrId::STR_STATE_ON : StrId::STR_STATE_OFF});
      if (bookEnabled) {
        items.push_back({FileBrowserAction::ReadingStats, StrId::STR_READING_STATS});
        items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
      }
    }
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
  }
  return items;
}

bool hasClearableBookCache(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

bool canSendNearby(const std::string& path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasTxtExtension(path) || FsHelpers::hasXtcExtension(path) ||
         FsHelpers::hasPngExtension(path) || FsHelpers::hasBmpExtension(path);
}

void clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crossdink").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
    ClippingStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  }
}

bool clearBookCache(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath)) {
    return clearBookCachePreservingUserState(fullPath);
  }
  return false;
}

bool deleteBookStats(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::remove(cachePath);
}

std::unique_ptr<Activity> createReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::string& fullPath, const std::string& title) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty() || !BookStatsTracking::isEnabled(cachePath)) {
    LOG_ERR("BookActions", "No reading stats for: %s", fullPath.c_str());
    return {};
  }
  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("BookActions", "Could not create stats cache for: %s", fullPath.c_str());
    return {};
  }

  const RecentBook book{fullPath, title, {}, {}};
  const float progress = FsHelpers::hasEpubExtension(fullPath) ? RecentBookProgress::loadCachedEpubPercent(book)
                                                               : RecentBookProgress::loadPercent(book);
  const BookReadingStats stats = BookReadingStats::load(cachePath);
  const GlobalReadingStats global = GlobalReadingStats::load();
  if (GlobalReadingStats::hasSyncedStats()) {
    return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0,
                                                global, GlobalReadingStats::loadAggregated(global));
  }
  return makeUniqueNoThrow<BookStatsActivity>(renderer, mappedInput, title, cachePath, stats, progress, false, 0,
                                              global);
}

bool resetBookReaderSettings(const std::string& fullPath) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }
  return EpubReaderActivity::resetBookReaderSettings(fullPath);
}

std::vector<std::string> epubRenderModeOptions() {
  return {I18N.get(StrId::STR_RENDER_MODE_CROSSDINK_DEFAULT), I18N.get(StrId::STR_RENDER_MODE_BALANCED),
          I18N.get(StrId::STR_RENDER_MODE_LIGHT)};
}

uint8_t epubRenderModeDisplayIndex(const uint8_t renderMode) {
  switch (static_cast<EpubRenderMode>(renderMode)) {
    case EpubRenderMode::Balanced:
      return 1;
    case EpubRenderMode::Light:
      return 2;
    case EpubRenderMode::CrossDinkDefault:
    default:
      return 0;
  }
}

uint8_t epubRenderModeForDisplayIndex(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return static_cast<uint8_t>(EpubRenderMode::Balanced);
    case 2:
      return static_cast<uint8_t>(EpubRenderMode::Light);
    case 0:
    default:
      return static_cast<uint8_t>(EpubRenderMode::CrossDinkDefault);
  }
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool isBookStatsTrackingEnabled(const std::string& fullPath) {
  return BookStatsTracking::isBookEnabled(bookStatsCachePath(fullPath));
}

bool toggleBookStatsTracking(const std::string& fullPath, bool& enabled) {
  if (!hasReadingStats(fullPath) || !SETTINGS.shouldTrackReadingStats()) return false;
  const std::string cachePath = bookStatsCachePath(fullPath);
  enabled = !BookStatsTracking::isBookEnabled(cachePath);
  if (BookStatsTracking::setBookEnabled(cachePath, enabled)) return true;
  enabled = BookStatsTracking::isBookEnabled(cachePath);
  return false;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed) {
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  if (!isEpub && !isXtc) {
    return false;
  }

  Epub epub(fullPath, "/.crossdink");
  Xtc xtc(fullPath, "/.crossdink");
  std::string cachePath;
  std::string title;
  std::string author;
  if (isEpub) {
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    title = epub.getTitle();
    author = epub.getAuthor();
  } else {
    if (!xtc.load()) {
      return false;
    }
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    title = xtc.getTitle();
    author = xtc.getAuthor();
  }

  BookReadingStats stats = BookReadingStats::load(cachePath);
  completed = !stats.isCompleted;
  stats.isCompleted = completed;
  const bool trackStats = BookStatsTracking::isEnabled(cachePath);
  if (completed && trackStats && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  if (!stats.save(cachePath)) {
    LOG_ERR("BookActions", "Could not save completion for: %s", fullPath.c_str());
    return false;
  }
  if (trackStats) {
    GlobalReadingStats globalStats = GlobalReadingStats::load();
    if (completed) {
      globalStats.completedBooks++;
    } else if (globalStats.completedBooks > 0) {
      globalStats.completedBooks--;
    }
    globalStats.save();
  }

  // Changing completion status does not open a book. The reader adds it to
  // recents if it is opened again after being marked unfinished.
  if (SETTINGS.removeReadBooksFromRecents && completed) RECENT_BOOKS.removeByPath(fullPath);

  if (isEpub && completed && SETTINGS.moveFinishedToReadFolder && fullPath.rfind("/Read/", 0) != 0) {
    const std::string oldCachePath = epub.getCachePath();
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!Storage.rename(fullPath.c_str(), dstPath.c_str())) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
               tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      return true;
    }

    BookMoveUtils::migrateMovedEpubState(fullPath, dstPath, oldCachePath, title, author,
                                         !SETTINGS.removeReadBooksFromRecents);
  }

  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
  bool runSharedAction(const FileBrowserAction action, const std::string& fullPath, const std::string& title,
                       const Host& host) {
    switch (action) {
      case FileBrowserAction::ToggleBookStatsTracking: {
        bool enabled = false;
        if (!toggleBookStatsTracking(fullPath, enabled)) {
          const std::string error = std::string(tr(STR_TRACK_READING_STATS)) + " " + tr(STR_FAILED_LOWER);
          showToast(host.renderer, error.c_str());
        }
        host.refresh();
        return true;
      }
      case FileBrowserAction::ToggleCompleted: {
        bool completed = false;
        if (toggleBookCompleted(fullPath, title, completed)) {
          showToast(host.renderer, completed ? tr(STR_MARKED_FINISHED) : tr(STR_MARKED_UNFINISHED));
        }
        host.refresh();
        return true;
      }
      case FileBrowserAction::ReadingStats:
        // The activity manager repaints the host when the stats screen closes.
        host.open(createReadingStatsActivity(host.renderer, host.mappedInput, fullPath, title),
                  [](const ActivityResult&) {});
        return true;
      case FileBrowserAction::DeleteCache:
        confirmThenRun(host, StrId::STR_DELETE_CACHE, fullPath, title, clearBookCache, StrId::STR_BOOK_CACHE_DELETED);
        return true;
      case FileBrowserAction::DeleteStats:
        confirmThenRun(host, StrId::STR_DELETE_BOOK_STATS, fullPath, title, deleteBookStats,
                       StrId::STR_BOOK_STATS_DELETED);
        return true;
      case FileBrowserAction::ResetReaderSettings:
        confirmThenRun(host, StrId::STR_RESET_BOOK_READER_SETTINGS, fullPath, title, resetBookReaderSettings,
                       StrId::STR_BOOK_READER_SETTINGS_RESET);
        return true;
      case FileBrowserAction::EpubRenderMode:
        host.open(
            makeUniqueNoThrow<OptionPopupActivity>(
                host.renderer, host.mappedInput, StrId::STR_EPUB_RENDER_MODE, epubRenderModeOptions(),
                epubRenderModeDisplayIndex(EpubReaderActivity::loadBookRenderMode(fullPath))),
            [host, fullPath](const ActivityResult& selectionResult) {
              const auto* selection = std::get_if<OptionSelectionResult>(&selectionResult.data);
              if (!selectionResult.isCancelled && selection != nullptr &&
                  !EpubReaderActivity::saveBookRenderMode(fullPath, epubRenderModeForDisplayIndex(selection->index))) {
                LOG_ERR("BookActions", "Failed to save render mode for: %s", fullPath.c_str());
              }
              host.refresh();
            });
        return true;
      case FileBrowserAction::SendNearby:
        activityManager.goToNearbyBookSend(fullPath, false);
        return true;
      default:
        return false;
    }
  }

}  // namespace BookActions
