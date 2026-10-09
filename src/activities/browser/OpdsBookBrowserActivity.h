#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <OpdsParser.h>

#include <atomic>
#include <bitset>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "OpdsBookDownloader.h"
#include "OpdsPageCache.h"
#include "OpdsPreloadPool.h"
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

  explicit OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, OpdsServer server,
                                   bool fromServerList = false);
  // Out of line: feedConnection's type is only forward-declared here.
  ~OpdsBookBrowserActivity() override;

  void onEnter() override;
  void onExit() override;
  bool usesWifi() const override { return true; }
  bool sharesWifiWithRemote() const override { return true; }
  void loop() override;
  void render(RenderLock&&) override;
  // Progress repaints come every few seconds: booster off between them.

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
#if defined(FREEINK_NET_WOLFSSL)
  // Kept-alive HTTPS connection for foreground feed fetches. Background
  // preloads use the pool's own connections.
  std::unique_ptr<freeink::SecureHttpClient> feedConnection;
  unsigned long feedConnectionLastUseMs = 0;
#endif
  // Set when Back (button or header tap) cancels a foreground feed fetch;
  // fetchFeed() then goes back instead of showing the fetch error.
  bool fetchCancelled = false;
  // Set by loadFeed(): the page came straight from the cache, not the network.
  bool shownFromCache = false;
  // The server list's background join is still running: loop() checks Wi-Fi
  // once it has settled instead of the Wi-Fi screen restarting the join.
  bool awaitingBackgroundJoin = false;
  // PSRAM devices only (null on C3): raw feed pages for Back/Prev, and the
  // background downloads of the next page and the first page's feeds.
  // Declared so the pool is destroyed (joined) before the cache.
  std::unique_ptr<OpdsPageCache> pageCache;
  std::unique_ptr<OpdsPreloadPool> preload;
  size_t entryCount = 0;
  // Whether entries[0] / entries[entryCount - 1] are the synthetic Prev / Next
  // page rows added from the feed's rel="previous" / rel="next" links.
  bool hasPrevPageRow = false;
  bool hasNextPageRow = false;
  // Feeds Back returns to, with the row and scroll position to restore there.
  struct HistoryEntry {
    std::string path;
    int selectorIndex = 0;
    int topIndex = 0;
  };
  std::vector<HistoryEntry> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  std::string searchDescriptionUrl;                   // OpenSearch description; fetched on first search
  std::bitset<MAX_OPDS_FEED_ENTRIES + 2> onSd;        // book rows already in the download folder
  std::bitset<MAX_OPDS_FEED_ENTRIES + 2> pageCached;  // feed rows whose page is in pageCache
  uint32_t pageCachedAt = 0;                          // pageCache->changes() when pageCached was set
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
  // Picked from the server list: Back at the root returns there, not Home.
  const bool fromServerList;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  // Last download frame sent to the panel (render task only). A re-render with
  // the same progress (the dialog closing right after the first frame) skips
  // its refresh.
  struct ShownDownloadFrame {
    bool valid = false;
    bool receiving = false;
    size_t progress = 0;
    size_t total = 0;
  };
  ShownDownloadFrame shownDownloadFrame;
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection
  // Set by the Cancel button handler; loop() forwards it to bookDownloader.
  bool cancelDownload = false;
  // Book the user chose to open after its download; onExit() reboots into it
  // when Wi-Fi cannot be left in place.
  std::string openAfterExit;

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
  // Skipped when path is already in the page cache: the list then replaces the
  // current screen directly.
  void showLoadingBeforeFetch(const std::string& path);
  void pushHistory() { navigationHistory.push_back(HistoryEntry{currentPath, selectorIndex, topIndex}); }
  // restoreRow/restoreTop: selection and scroll to show once loaded (Back).
  // recheck: refetch a page shown from the cache in the background (off for
  // the redraw a recheck itself triggers).
  void fetchFeed(const std::string& path, int restoreRow = 0, int restoreTop = 0, bool recheck = true);
  // Fills parser from the PSRAM cache, a finished prefetch, or the network
  // (caching the response). False only on a network failure.
  bool loadFeed(const std::string& url, OpdsParser& parser);
  bool pollFetchCancel();
  void startNextPagePrefetch(const std::string& nextHref);
  // First page only: queues every navigation row's feed for the preload pool.
  void preloadFeedsOnPage();
  void stopPrefetch();
  // The shared feed connection for the next request, or null. Drops a
  // connection idle long enough that a router or server may have silently
  // forgotten it (a dead socket would stall the request for its full timeout).
  freeink::SecureHttpClient* feedConnectionForRequest();
  bool ensureEntryBuffer();
  void clearEntries();
  bool appendEntry(OpdsEntry&& entry);
  // pageLink: the synthetic Prev/Next page row, which replaces the current
  // listing instead of pushing it onto the Back history.
  void navigateToEntry(const OpdsEntry& entry, bool pageLink);
  void leaveBrowser();  // server list when picked from it, else Home
  void navigateBack();
  // Asks before replacing a book already on SD (showing its size and date),
  // otherwise downloads straight away.
  void requestDownload(const OpdsEntry& book);
  // URL, credentials and auth origin for downloading book from the current feed.
  OpdsBookDownloader::Request bookRequest(const OpdsEntry& book) const;
  // ConfirmationActivity note poll: the size probe's result once it finishes.
  static bool pollDownloadSize(void* self, std::string& body);
  // filename: the SD destination from requestDownload. resumeValidator: set
  // on Retry to continue the failed attempt's .part file (may be empty).
  void downloadBook(const OpdsEntry& book, const std::string& filename, const std::string* resumeValidator = nullptr);
  // DOWNLOADING state: forwards cancel input, redraws progress, and finishes
  // once the background task has exited.
  void pollDownload();
  // After a finished download: asks whether to open the book now.
  void offerToOpen(const std::string& path);
  // After a failed download: Retry resumes the same book (from byte 0 when the
  // server cannot), Cancel removes the partial file and returns to the listing.
  void offerRetry(const std::string& path);
  void markBooksOnSd();
  void markCachedFeeds();
  bool hasSearch() const { return !searchTemplate.empty() || !searchDescriptionUrl.empty(); }
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
  // While the list just sits there, the loop may drop the CPU clock and light
  // sleep with Wi-Fi up (each fetch turns Wi-Fi power save off for itself).
  bool allowsRadioIdleSleep() override;
};
