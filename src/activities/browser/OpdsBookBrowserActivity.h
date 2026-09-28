#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <OpdsParser.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "OpdsBookDownloader.h"
#include "OpdsPageCache.h"
#include "OpdsPagePrefetcher.h"
#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "util/ButtonNavigator.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // FreeInkUI app runtime for the browsing screen: owns the interaction table,
  // routes touch snapshots, and dispatches row/search actions to the static
  // handlers below. 24 interaction slots cover the densest page (Small scale,
  // ~12 rows) plus the header's search button, with headroom.
  using UiApp = freeink::ui::FreeInkApp<24, 4>;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::unique_ptr<OpdsEntry[]> entries;
  // PSRAM devices only (null on C3): raw feed pages for Back/Prev, and the
  // background download of the next page. Declared so the prefetcher is
  // destroyed (joined) before the cache.
  std::unique_ptr<OpdsPageCache> pageCache;
  std::unique_ptr<OpdsPagePrefetcher> prefetcher;
  size_t entryCount = 0;
  // Whether entries[0] / entries[entryCount - 1] are the synthetic Prev / Next
  // page rows added from the feed's rel="previous" / rel="next" links.
  bool hasPrevPageRow = false;
  bool hasNextPageRow = false;
  std::vector<std::string> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  int selectorIndex = 0;
  std::string errorMessage;
  std::string statusMessage;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  // False until the first body byte: the screen says Connecting meanwhile.
  bool downloadReceiving = false;
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  // Book downloads run on a background task; loop() polls it for progress
  // and completion and forwards cancel requests.
  OpdsBookDownloader bookDownloader;

  OpdsServer server;  // Copied at construction — safe even if the store changes during browsing

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  // Set by the Cancel button handler; loop() forwards it to bookDownloader.
  bool cancelDownload = false;

  // Single screen fn dispatching on `state`: every state shares the themed
  // header and gets built through FreeInkUI.
  static void rootScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSearchEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onCancelEvent(const freeink::ui::ActionEvent& event, void* user);
  void screenHeader(UiApp::ScreenType& screen, bool withSearch);
  void buildBrowsingScreen(UiApp::ScreenType& screen);
  void buildDownloadScreen(UiApp::ScreenType& screen);
  void buildStatusScreen(UiApp::ScreenType& screen);
  void activateSelected();

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void showLoadingBeforeFetch();
  void fetchFeed(const std::string& path);
  // Fills parser from the PSRAM cache, a finished prefetch, or the network
  // (caching the response). False only on a network failure.
  bool loadFeed(const std::string& url, OpdsParser& parser);
  void startNextPagePrefetch(const std::string& nextHref);
  void stopPrefetch();
  bool ensureEntryBuffer();
  void clearEntries();
  bool appendEntry(OpdsEntry&& entry);
  // pageLink: the synthetic Prev/Next page row, which replaces the current
  // listing instead of pushing it onto the Back history.
  void navigateToEntry(const OpdsEntry& entry, bool pageLink);
  void navigateBack();
  // Asks before replacing a book already on SD (showing its size and date),
  // otherwise downloads straight away.
  void requestDownload(const OpdsEntry& book);
  // filename: the SD destination from requestDownload.
  void downloadBook(const OpdsEntry& book, const std::string& filename);
  // DOWNLOADING state: forwards cancel input, redraws progress, and finishes
  // once the background task has exited.
  void pollDownload();
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
};
