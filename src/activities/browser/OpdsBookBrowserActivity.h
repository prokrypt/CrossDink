#pragma once
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <Memory.h>
#include <OpdsParser.h>

#include <array>
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
#include "components/UiAppHost.h"
#include "util/ButtonNavigator.h"
#include "util/OpdsFeedError.h"

/**
 * Activity for browsing and downloading books from an OPDS server.
 * Supports navigation through catalog hierarchy and downloading EPUBs.
 */
class OpdsBookBrowserActivity final : public Activity {
 public:
  enum class BrowserState { CHECK_WIFI, WIFI_SELECTION, LOADING, BROWSING, DOWNLOADING, ERROR, SEARCH_INPUT };
  // Rows held in memory at a time (the array lands in PSRAM): a window over
  // the feed's pages that follows the view; pages outside it re-parse from
  // the page cache when scrolled back to.
  static constexpr size_t ENTRY_CAPACITY = 2000;

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
  using UiApp = UiAppList;

  ButtonNavigator buttonNavigator;
  BrowserState state = BrowserState::LOADING;
  ScreenTransitionRefresh screenTransitionRefresh;
  std::unique_ptr<OpdsEntry[]> entries;  // the window: rows [windowBase, windowBase + entryCount)
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
  // An uncached page downloading on a pool worker while the loop runs: rows
  // show as soon as the first screenful has parsed, the rest land silently.
  // parser (null: none) reads into `entries`, so it is declared after them.
  struct StreamFetch {
    std::string url;
    std::string path;
    size_t base = 0;     // list rows before this page (0: the feed's first page)
    size_t drained = 0;  // page bytes handed to the parser so far
    int restoreRow = 0;
    int restoreTop = 0;
    bool recheck = true;
    std::unique_ptr<OpdsParser> parser;
  };
  StreamFetch stream;
  size_t entryCount = 0;
  // Catalog size from the feed's opensearch:totalResults; -1 when not given.
  int32_t feedTotal = -1;
  // One continuous list per feed. Every page seen so far, in feed order
  // (strings in PSRAM: a long browse sees hundreds); list indices (selection,
  // scroll, taps) are absolute rows over these.
  struct PageInfo {
    PsramString path;
    uint32_t firstRow = 0;
    uint16_t rows = 0;
  };
  std::vector<PageInfo> pages;
  size_t windowBase = 0;  // absolute row of entries[0]
  // The page behind the last known one (its rel="next" link, absolute),
  // appended once the view nears the end of the list; empty when there is
  // none or the list is full.
  std::string nextPagePath;
  uint32_t appendRetryMs = 0;  // millis() | 1 after a failed append (0: none)
  // Feeds Back returns to, with the row and scroll position to restore there.
  struct HistoryEntry {
    std::string path;
    int selectorIndex = 0;
    int topIndex = 0;
  };
  std::vector<HistoryEntry> navigationHistory;
  std::string currentPath;
  std::string searchTemplate;
  std::string searchDescriptionUrl;        // OpenSearch description; fetched on first search
  std::bitset<ENTRY_CAPACITY> onSd;        // book rows already in the download folder
  std::bitset<ENTRY_CAPACITY> pageCached;  // feed rows whose page is in pageCache
  // Sorted keys of every book in the Library index (folded title + author key,
  // and folded basename), so rows match books in any folder. Empty without an index.
  std::vector<uint64_t, PsramPreferAllocator<uint64_t>> libraryKeys;
  std::array<char, 16> countLabel{};  // scratch for the row provider's count label
  uint32_t pageCachedAt = 0;          // pageCache->changes() when pageCached was set
  uint32_t marksPendingMs = 0;        // millis() | 1 when pageCached first changed unshown
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
  bool leavingToList = false;  // Back to the server list: onExit hands the page cache over

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
  static void provideRow(void* user, uint16_t index, freeink::ui::ListItem& item);
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
  // Starts a feed: its first page replaces the list. stream: an uncached
  // page may show progressively (see StreamFetch); off for the blocking retry
  // after a streamed fetch failed.
  void fetchFeed(const std::string& path, int restoreRow = 0, int restoreTop = 0, bool recheck = true,
                 bool stream = true);
  // Loads one page into the list at row `base` (0 replaces the list): from
  // the cache, streamed, or fetched in the foreground.
  void loadPage(const std::string& url, const std::string& pagePath, size_t base, int restoreRow, int restoreTop,
                bool recheck, bool stream);
  // Appends the page behind the list (nextPagePath), if any.
  void appendNextPage(bool stream);
  // The parsed page joins the listing: rows, marks, selection, next page.
  void finishFeed(const OpdsParser& parser, const std::string& url, const std::string& pagePath, size_t base,
                  int restoreRow, int restoreTop, bool recheck);
  bool streaming() const { return stream.parser != nullptr; }
  // True when url is downloading on a pool worker and the parser is set up.
  bool startStream(const std::string& url, const std::string& path, size_t base, int restoreRow, int restoreTop,
                   bool recheck);
  // Each loop tick: parses the bytes received so far, shows rows once a
  // screenful is in, finishes the feed when the download ends.
  void pumpStream();
  // Shows the rows parsed so far. done: the whole page has parsed.
  void publishStreamedRows(bool done);
  void abortStream();
  // Fills parser from the PSRAM cache, a finished prefetch, or the network
  // (caching the response). False only on a network failure.
  bool loadFeed(const std::string& url, OpdsParser& parser, OpdsFeedError* error = nullptr);
  bool pollFetchCancel();
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
  void navigateToEntry(const OpdsEntry& entry);
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
  // serverFilename: save under the server's name in filename's folder instead.
  void downloadBook(const OpdsEntry& book, const std::string& filename, const std::string* resumeValidator = nullptr,
                    bool serverFilename = false);
  // DOWNLOADING state: forwards cancel input, redraws progress, and finishes
  // once the background task has exited.
  void pollDownload();
  // After a finished download: asks whether to open the book now.
  void offerToOpen(const std::string& path);
  // After a failed download: Retry resumes the same book (from byte 0 when the
  // server cannot), Cancel removes the partial file and returns to the listing.
  void offerRetry(const std::string& path);
  // The server's file name is already on SD: Confirm replaces that file.
  void offerOverwrite(const std::string& path);
  // Window rows [from, to) (window-relative); the other marks stay.
  void markBooksOnSd(size_t from = 0, size_t to = SIZE_MAX);
  void loadLibraryKeys();
  // Rows known so far (the list length); at most what the list can index.
  size_t knownRows() const;
  // The entry at an absolute row, or null when it is outside the window.
  const OpdsEntry* rowEntry(int row) const;
  const PageInfo* pageStartingAt(uint32_t row) const;
  const PageInfo* pageEndingAt(uint32_t row) const;
  // Drop whole pages at the window's front (back) so `rows` more fit, never
  // a page the view is on. Returns the room left.
  size_t makeRoom(size_t rows, bool atFront);
  // Shifts the window rows and their marks by `delta` (positive: towards
  // higher indices), adjusting entryCount and windowBase. Render lock held.
  void shiftWindow(int delta, size_t fromRow);
  // Re-parses a known page (from the cache, else the network, blocking) into
  // the window: at its end, or at its front when `front`.
  void reloadPage(const PageInfo& page, bool front);
  // Each tick while browsing: keeps the window covering the view plus a few
  // screens ahead and behind, loading or dropping one page per call.
  void keepWindowAroundView();
  bool markCachedFeeds();  // true when a row's mark changed
  bool hasSearch() const { return !searchTemplate.empty() || !searchDescriptionUrl.empty(); }
  void launchSearch();
  void performSearch(const std::string& query);
  bool preventAutoSleep() override;
  // While the list just sits there, the loop may drop the CPU clock and light
  // sleep with Wi-Fi up (each fetch turns Wi-Fi power save off for itself).
  bool allowsRadioIdleSleep() override;
};
