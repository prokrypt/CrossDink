#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <LibraryIndexFile.h>

#include <memory>
#include <string>

#include "RecentBooksStore.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

class LibraryActivity final : public Activity {
 public:
  LibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool blocksGlobalInput() const override { return actionPopup.isActive(); }

 private:
  enum class Sort : uint8_t { DateAdded, Title, AuthorLast, AuthorFirst, RecentlyRead, Series, Genre };
  // Touch header controls precede the book rows; button-only navigation visits books directly.
  static constexpr int CONTROL_COUNT = 5;
  using UiApp = freeink::ui::FreeInkApp<32, 4>;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  ButtonNavigator buttonNavigator;
  freeink::ui::ListNav listNav;
  OptionPopup actionPopup;
  library::LibraryIndexFile index;
  Sort sort = Sort::RecentlyRead;
  bool descending = true;
  int selection = CONTROL_COUNT;
  bool showSelection = true;
  int topIndex = 0;
  bool uiReady = false;
  bool longPressFired = false;
  bool ignoreConfirmRelease = false;
  bool scanFailed = false;
  bool filterFailed = false;
  bool pendingCacheDeletedFeedback = false;
  unsigned long cacheDeletedFeedbackShowTime = 0;
  std::string query;
  // Searches and file-type filters allocate one u16 per visible source book, fallibly.
  std::unique_ptr<uint16_t[]> filtered;
  uint16_t filteredCount = 0;
  uint16_t recentRows[RecentBooksStore::MAX_RECENT_BOOKS]{};
  size_t recentCount = 0;
  // SDK rowProvider consumes the strings before asking for the next row.
  // Reuse one row instead of retaining every title/author in the library.
  RecentBook rowScratch;
  std::string groupHeading;
  std::string previousGroupScratch;
  std::string groupKeyScratch;
  std::string previousGroupKeyScratch;
  std::string seriesScratch;
  std::string genreScratch;
  std::string subtitleScratch;
  // Display text of recently drawn rows, direct-mapped by row. Moving the
  // selection or re-running layout passes then redraws without re-reading the
  // index from the card. Dropped whenever the row order or contents change.
  struct CachedRow {
    int32_t row = -1;
    bool ok = false;
    bool hasHeading = false;
    uint8_t icon = 0;
    std::string title;
    std::string subtitle;
    std::string heading;
  };
  static constexpr int ROW_CACHE_SIZE = 16;
  std::unique_ptr<CachedRow[]> rowCache;
  // Used when the cache could not be allocated.
  CachedRow uncachedRow;
  // Input seen since the last processed frame. Rendering holds the render lock
  // through the panel refresh, and waiting on it here would stall the main
  // loop's touch polling: a swipe made during a refresh then loses its start
  // point and can land as a tap on a row. Input is latched every loop and
  // applied once the frame is out.
  struct PendingInput {
    bool touchPress = false;
    bool touchRelease = false;
    bool longPress = false;
    bool headerBack = false;
    bool confirmReleased = false;
    bool backReleased = false;
    bool leftReleased = false;   // button-only: sort picker
    bool rightReleased = false;  // button-only: menu
    int16_t pressX = 0;
    int16_t pressY = 0;
    int16_t releaseX = 0;
    int16_t releaseY = 0;
    int8_t scrollPages = 0;  // positive moves down the list
    int8_t steps = 0;
    int8_t pageSteps = 0;
  };
  PendingInput pending;
  // Travel of the current contact, so a drag is not taken as a tap.
  bool touchTracking = false;
  int touchStartX = 0;
  int touchStartY = 0;
  int touchLastX = 0;
  int touchLastY = 0;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onControlEvent(const freeink::ui::ActionEvent& event, void* user);
  static void provideRow(void* user, uint16_t row, freeink::ui::ListItem& item);
  void latchInput();
  void buildListScreen(UiApp::ScreenType& screen);
  void buildSortHeader(UiApp::ScreenType& screen);
  const char* sortLabel() const;
  library::SortOrder indexOrder() const;
  int rowCount() const;
  uint16_t ordinalForRow(int row);
  bool readBook(int row, RecentBook& book, bool fullPath = true);
  uint32_t groupForRow(int row);
  uint16_t dateGroupForRow(int row);
  bool metadataGroupForRow(int row, std::string& out);
  bool hasActiveFilter() const;
  // Rescans the card only when storage reports a Library-visible change since
  // the last successful scan (or `force`); otherwise reopens the saved index.
  bool rebuildIndex(bool showScanning, bool force = false);
  CachedRow& rowFor(int row);
  void fillRow(int row, CachedRow& out);
  void invalidateRowCache();
  void resolveRecents();
  void applyFilter();
  void resetViewport();
  void reloadAfterBookAction();
  void openBook(int row);
  void openSortPicker(int selectedIndex = -1);
  void openMenu();
  void refreshLibrary();
  void openSearch();
  void openSettings();
  void activateControl(int control);
  // Owns the allocation-failure check and render lock for every child result.
  void openDialog(std::unique_ptr<Activity>&& activity, ActivityResultHandler handler);
  void promptDeleteBook(const RecentBook& book);
  void promptRemoveBook(const std::string& path, const std::string& title);
  void showBookActionMenu(size_t bookIndex, bool ignoreInitialConfirmRelease = false);
};
