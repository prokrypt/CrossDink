#include "OpdsBookBrowserActivity.h"

#include <Arduino.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Knobs.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <OpdsStream.h>
#if defined(FREEINK_NET_WOLFSSL)
#include <SecureHttpClient.h>
#endif
#include <Utf8.h>
#include <WiFi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "OpdsPageCache.h"
#include "OpdsPreloadPool.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/listIcons.h"
#include "components/icons/markIcons.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/WifiBackgroundJoin.h"
#include "util/BookCacheUtils.h"
#include "util/DaylightSaving.h"
#include "util/OpdsDownloadFolder.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
// Plain allocations this large land in PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL).
constexpr size_t OPDS_BROWSER_ENTRY_CAPACITY = OpdsBookBrowserActivity::ENTRY_CAPACITY;
// The next page is appended once the view is within this many screens of
// the end of the list.
constexpr int APPEND_AHEAD_SCREENS = 2;
constexpr uint32_t APPEND_RETRY_MS = 10000;
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_SEARCH = 2;
constexpr fui::ActionId ACTION_CANCEL = 3;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;
// A kept-alive feed connection idle longer than this is closed before the
// next request. Some servers and load balancers drop an idle socket without a
// FIN; the request then waits out the whole header timeout before the retry
// on a fresh connection. Reuse after 1.4 s and 3.5 s idle worked on
// mayberry.pub, after 10.5 s it never answered (crash log 2026-09-30).
KNOB_ALIAS(OPDS_KEEPALIVE_MAX_IDLE_MS, opdsKeepaliveMs);  // Goodies > Knobs

std::string buildBookFilenameBase(const OpdsEntry& book, const OpdsFilenameFormat format) {
  const std::string title(book.title);
  const std::string author(book.author);
  if (author.empty()) return title;
  if (title.empty()) return author;
  if (format == OpdsFilenameFormat::TITLE_AUTHOR) return title + " - " + author;
  return author + " - " + title;
}

// Folder books from server download to: its own folder, else the shared
// default; "/" for the SD root, empty when the configured folder is invalid.
std::string downloadFolderFor(const OpdsServer& server) {
  std::string folder;
  if (!OpdsDownloadFolder::normalize(OpdsDownloadFolder::effective(server.downloadFolder, SETTINGS.opdsDownloadFolder),
                                     folder)) {
    LOG_ERR("OPDS", "Invalid download folder");
    return {};
  }
  return folder.empty() ? "/" : folder;
}

// SD path a book downloads to: folder (from downloadFolderFor) plus the
// sanitized "<author> - <title>.epub" name.
std::string bookDownloadPath(const OpdsEntry& book, const OpdsFilenameFormat format, const std::string& folder) {
  std::string path;
  path.reserve(96);
  path += folder;
  if (path != "/") path += '/';
  path += StringUtils::sanitizeFilename(buildBookFilenameBase(book, format));
  path += ".epub";
  return path;
}

// Library index match keys. The title key uses the index's own fold and author
// key, cut exactly as the builder stores them, so an OPDS row hashes like the
// record of the same book wherever it sits on the card.
uint64_t libraryTitleKey(const std::string& title, const std::string& author) {
  std::string key = library::fold(title);
  key.resize(utf8SafeTruncateBuffer(key.data(), static_cast<int>(std::min(key.size(), library::CLIX_FOLD_BYTES))));
  key += '\0';
  // The builder files "Unknown" authors as no author.
  if (library::fold(author) != "unknown") key += library::authorKey(author).substr(0, library::CLIX_AUTHOR_KEY_BYTES);
  return library::clixPathHash(key.data(), key.size());
}

uint64_t libraryNameKey(const std::string& name) {
  const std::string folded = library::fold(name);
  return library::clixPathHash(folded.data(), folded.size()) ^ 1;  // distinct from title keys
}

// "12.3 MB", "456 KB" or "789 B", with integer math only.
void formatFileSize(const uint64_t bytes, char* out, const size_t outSize) {
  constexpr uint64_t KB = 1024;
  constexpr uint64_t MB = KB * KB;
  const uint64_t roundedKb = (bytes + KB / 2) / KB;
  if (roundedKb >= KB) {
    const uint64_t tenths = (bytes * 10 + MB / 2) / MB;
    snprintf(out, outSize, "%lu.%lu MB", static_cast<unsigned long>(tenths / 10),
             static_cast<unsigned long>(tenths % 10));
  } else if (bytes >= KB) {
    snprintf(out, outSize, "%lu KB", static_cast<unsigned long>(roundedKb));
  } else {
    snprintf(out, outSize, "%lu B", static_cast<unsigned long>(bytes));
  }
}

// Packed FAT date/time (HalFile::modificationTime) as "YYYY-MM-DD HH:MM",
// decoded by hand rather than through strftime. False when the file has none.
bool formatFatDateTime(const uint32_t packed, char* out, const size_t outSize) {
  if (packed == 0) return false;
  const uint16_t date = static_cast<uint16_t>(packed >> 16);
  const uint16_t time = static_cast<uint16_t>(packed & 0xFFFF);
  const uint16_t year = static_cast<uint16_t>(1980 + (date >> 9));
  const uint8_t month = static_cast<uint8_t>((date >> 5) & 0x0F);
  const uint8_t day = static_cast<uint8_t>(date & 0x1F);
  const unsigned hour = time >> 11;
  const unsigned minute = (time >> 5) & 0x3F;
  if (!DaylightSaving::isValidDate(year, month, day) || hour > 23 || minute > 59) return false;
  snprintf(out, outSize, "%04u-%02u-%02u %02u:%02u", static_cast<unsigned>(year), static_cast<unsigned>(month),
           static_cast<unsigned>(day), hour, minute);
  return true;
}

// Mayberry prefixes folder titles with U+1F4C1 (file folder), which the UI
// fonts lack; show "/name" instead.
void replaceFolderEmoji(PsramString& title) {
  constexpr char FOLDER_EMOJI[] = "\xF0\x9F\x93\x81";
  constexpr size_t FOLDER_EMOJI_LEN = sizeof(FOLDER_EMOJI) - 1;
  if (title.compare(0, FOLDER_EMOJI_LEN, FOLDER_EMOJI) != 0) return;
  size_t prefixLen = FOLDER_EMOJI_LEN;
  while (prefixLen < title.size() && title[prefixLen] == ' ') ++prefixLen;
  title.replace(0, prefixLen, "/");
}

}  // namespace

OpdsBookBrowserActivity::OpdsBookBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                 OpdsServer server, const bool fromServerList)
    : Activity("OpdsBookBrowser", renderer, mappedInput),
      buttonNavigator(),
      server(std::move(server)),
      fromServerList(fromServerList),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

OpdsBookBrowserActivity::~OpdsBookBrowserActivity() = default;

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  sdFontSystem.releaseLoadedFont(renderer);

  state = BrowserState::CHECK_WIFI;
  entryCount = 0;
  navigationHistory.clear();
  searchTemplate = "";
  searchDescriptionUrl.clear();
  currentPath = "";
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = tr(STR_CHECKING_WIFI);

  uiReady = false;
  visibleRows = 1;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &OpdsBookBrowserActivity::onRowEvent, this);
  app.on(ACTION_SEARCH, &OpdsBookBrowserActivity::onSearchEvent, this);
  app.on(ACTION_CANCEL, &OpdsBookBrowserActivity::onCancelEvent, this);
  app.setScreen(&OpdsBookBrowserActivity::rootScreen, this);
  requestUpdate();

  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  loadLibraryKeys();

  if (psramHeapAvailable()) {
    // The server list may have prefetched this server's root page already.
    pageCache = opds_page_cache_handoff::take();
    const size_t budget = std::min(OPDS_PAGE_CACHE_MAX_BYTES, byteHeapSnapshot(MemoryPool::Psram).free / 4);
    if (!pageCache) pageCache = makeUniqueNoThrow<OpdsPageCache>(budget);
    if (pageCache) {
      opds_page_cache_handoff::restoreAfterRestart(*pageCache);
      preload = makeUniqueNoThrow<OpdsPreloadPool>(*pageCache, OPDS_PAGE_MAX_BYTES, server.username, server.password,
                                                   UrlUtils::ensureProtocol(server.url));
    }
    LOG_DBG("OPDS", "Page cache %s (budget %zu bytes)", pageCache ? "on" : "off", budget);
  }

#ifdef SIMULATOR
  // Use deterministic catalog data so the UI can be exercised without WiFi or an OPDS server.
  fetchFeed(currentPath);
#else
  awaitingBackgroundJoin = wifi_background_join::joining();
  if (!awaitingBackgroundJoin) checkAndConnectWifi();
#endif
}

void OpdsBookBrowserActivity::onExit() {
  library::invalidateLibraryIndex();
  Activity::onExit();
  // A book download in flight is cancelled (its .part file removed) and
  // joined before Wi-Fi goes down.
  bookDownloader.cancel();
  bookDownloader.join();
  // Joins the background download before Wi-Fi goes down and before the
  // cache it would hand its page to is freed.
  preload.reset();
  // Back to the server list: it reuses the pages fetched so far (its roots
  // included) instead of fetching every server again.
  if (leavingToList) {
    opds_page_cache_handoff::give(std::move(pageCache));
  } else {
    pageCache.reset();
  }
#if defined(FREEINK_NET_WOLFSSL)
  feedConnection.reset();  // closes the kept-alive socket before Wi-Fi goes down
#endif
  clearEntries();
  entries.reset();
  libraryKeys.clear();
  libraryKeys.shrink_to_fit();
  navigationHistory.clear();

#ifndef SIMULATOR
  // OPDS launches from minimal network boot, so the full app state is
  // restored even if setup failed before WiFi was started.
  leaveNetworkAfterExit(std::move(openAfterExit));
#endif
}

void OpdsBookBrowserActivity::activateSelected() {
  const OpdsEntry* entry = rowEntry(selectorIndex);
  if (!entry) return;  // outside the window: keepWindowAroundView() brings it in
  if (entry->type == OpdsEntryType::BOOK) {
    requestDownload(*entry);
    return;
  }
  navigateToEntry(*entry);
}

void OpdsBookBrowserActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  if (event.value < 0 || event.value >= static_cast<int>(self->knownRows())) return;
  self->selectorIndex = event.value;
  // The tapped row leaves the screen either way (new feed or download view);
  // a lingering tap flash would gray an unrelated row on the next list.
  self->app.clearTapFlash();
  self->activateSelected();
}

void OpdsBookBrowserActivity::onSearchEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::BROWSING) return;
  self->app.clearTapFlash();
  self->launchSearch();
}

void OpdsBookBrowserActivity::onCancelEvent(const fui::ActionEvent&, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  if (self->state != BrowserState::DOWNLOADING) return;
  self->app.clearTapFlash();
  self->cancelDownload = true;
}

void OpdsBookBrowserActivity::loop() {
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) {
    return;
  }
  if (streaming()) pumpStream();

  if (state == BrowserState::ERROR) {
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      navigateBack();
      return;
    }
    int tx = 0;
    int ty = 0;
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(tx, ty)) {
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        // Retry goes back to the server, not to a cached (e.g. empty) page.
        if (pageCache) pageCache->erase(UrlUtils::buildUrl(server.url, currentPath));
        showLoadingBeforeFetch(currentPath);
        fetchFeed(currentPath);
      } else {
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      state == BrowserState::CHECK_WIFI ? leaveBrowser() : navigateBack();
      return;
    }
    if (awaitingBackgroundJoin && !wifi_background_join::joining()) {
      awaitingBackgroundJoin = false;
      checkAndConnectWifi();
    }
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
#ifdef SIMULATOR
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) app.route(snap);
    }
    if (cancelDownload || mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      cancelDownload = false;
      state = BrowserState::BROWSING;
      requestUpdate();
    }
#else
    pollDownload();
#endif
    return;
  }

  if (state == BrowserState::BROWSING) {
    if (!streaming()) keepWindowAroundView();
    // Queued feed downloads start only from the browsing list; a network
    // fetch (the streamed page included) or a book download pauses them.
    if (preload && !streaming()) {
      preload->pump();
      // A preload stored or evicted a page: recheck the row marks and redraw
      // them in batches (as the server list): a redraw per landed page cost
      // extra e-ink refreshes and queued taps behind them. A repaint the user
      // causes meanwhile shows the marks landed so far.
      if (pageCache && pageCache->changes() != pageCachedAt && markCachedFeeds() && marksPendingMs == 0) {
        marksPendingMs = millis() | 1;
      }
      if (preload->marksDue(marksPendingMs)) requestUpdate();
      // A recheck found the shown page changed: re-parse it in place.
      std::string changed;
      if (preload->takeChange(changed) && changed == UrlUtils::buildUrl(server.url, currentPath)) {
        LOG_INF("OPDS", "Recheck: redrawing changed page");
        fetchFeed(currentPath, selectorIndex, topIndex, false);
      }
    }
    if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
      navigateBack();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      activateSelected();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (hasSearch() && selectorIndex == 0) launchSearch();
    }

    // Touch goes through the FreeInkApp: render() registered every tap target
    // (rows, header search button); route the snapshot and let the registered
    // handlers dispatch.
    if (uiReady) {
      const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
      if (snap.touchPressed || snap.touchReleased) {
        const auto event = app.route(snap);
        // No pressed-state repaint: the render it triggers would drop a slow
        // tap's release inside the uiReady window (tap-to-activate needed two
        // taps), and it costs a second e-ink refresh per tap.
        if (app.invalidated()) requestUpdate();
        if (event) return;  // dispatched to onRowEvent/onSearchEvent
        if (state != BrowserState::BROWSING) return;
      }
    }

    const int rows = static_cast<int>(knownRows());
    if (rows > 0) {
      // Swipes scroll the viewport; the selection stays put (it may scroll
      // off-screen) and button navigation pulls the view back to it.
      const auto swipe = mappedInput.wasSwipe();
      if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
        const int next = scrollListBy(topIndex, delta, visibleRows, rows);
        if (next != topIndex) {
          topIndex = next;
          requestUpdate();
        }
        return;
      }

      const auto moveSelection = [this, rows](const int index) {
        selectorIndex = index;
        topIndex = followListSelection(selectorIndex, topIndex, visibleRows, rows);
        requestUpdate();
      };
      buttonNavigator.onNextRelease(
          [this, rows, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, rows)); });
      buttonNavigator.onPreviousRelease(
          [this, rows, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, rows)); });
      buttonNavigator.onNextContinuous([this, rows, &moveSelection] {
        moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, rows, visibleRows));
      });
      buttonNavigator.onPreviousContinuous([this, rows, &moveSelection] {
        moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, rows, visibleRows));
      });
    }
  }
}

bool OpdsBookBrowserActivity::preventAutoSleep() {
  if (streaming()) return true;  // keeps the fast loop tick that feeds the parser
  switch (state) {
    case BrowserState::CHECK_WIFI:
    case BrowserState::WIFI_SELECTION:
    case BrowserState::LOADING:
    case BrowserState::DOWNLOADING:
    case BrowserState::SEARCH_INPUT:
      return true;
    case BrowserState::BROWSING:
    case BrowserState::ERROR:
      return false;
  }
  return false;
}

bool OpdsBookBrowserActivity::allowsRadioIdleSleep() {
  if (state != BrowserState::BROWSING && state != BrowserState::ERROR) return false;
  return !(preload && preload->busy());
}

void OpdsBookBrowserActivity::rootScreen(UiApp::ScreenType& screen, void* user) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(user);
  switch (self->state) {
    case BrowserState::BROWSING:
      self->buildBrowsingScreen(screen);
      break;
    case BrowserState::DOWNLOADING:
      self->buildDownloadScreen(screen);
      break;
    default:
      self->buildStatusScreen(screen);
      break;
  }
}

// Shared chrome for every state: reserve the firmware's button-hint band and
// draw the themed header (padding, centering, and rule come from the theme).
void OpdsBookBrowserActivity::screenHeader(UiApp::ScreenType& screen, const bool withSearch) {
  screen.takeBottom(static_cast<int16_t>(UITheme::getButtonHintsReserve(renderer)));
  const char* title = server.name.empty() ? tr(STR_OPDS_BROWSER) : server.name.c_str();
  // Browsing a feed that gives its catalog size: "<name> (<total>)".
  char titleWithTotal[96];
  if (withSearch && feedTotal >= 0) {
    snprintf(titleWithTotal, sizeof(titleWithTotal), "%s (%ld)", title, static_cast<long>(feedTotal));
    title = titleWithTotal;
  }
  // One header for every state, so the title and status row never jump. On
  // touch its arrow is Back, or Cancel while downloading.
  const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    const auto backLayout = TouchHeaderBackButton::layout(headerRect);
    const bool showSearch = withSearch && hasSearch();
    TouchHeaderBackButton::draw(renderer, uiTarget, headerRect, title, false,
                                showSearch ? static_cast<int>(backLayout.iconRect.width + 8) : 0);
    screen.takeTop(static_cast<int16_t>(headerRect.y + headerRect.height));

    if (showSearch) {
      fui::ButtonProps search;
      search.action = ACTION_SEARCH;
      search.styles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
      search.minTouchSize = screen.theme().minTouchSize;
      search.radius = 8;
      const fui::Rect searchRect{static_cast<int16_t>(headerRect.x + headerRect.width - backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.y),
                                 static_cast<int16_t>(backLayout.iconRect.width),
                                 static_cast<int16_t>(backLayout.iconRect.height)};
      screen.button(search, searchRect);
      // Keep the touch target clear of the divider, but draw the glyph on the
      // shared Back/title baseline instead of at the top of its action lane.
      const int16_t iconX =
          static_cast<int16_t>(searchRect.x + (searchRect.width - TouchHeaderBackButton::ICON_SIZE) / 2);
      const int16_t iconY = static_cast<int16_t>(backLayout.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                                                 (backLayout.iconRect.height - TouchHeaderBackButton::ICON_SIZE) / 2);
      screen.target().bitmap(
          fui::Rect{iconX, iconY, TouchHeaderBackButton::ICON_SIZE, TouchHeaderBackButton::ICON_SIZE},
          fui::bitmapFromIcon(icon_search_32), fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black));
    }
  } else {
    GUI.drawHeader(renderer, headerRect, title);
    screen.takeTop(static_cast<int16_t>(headerRect.y + headerRect.height));
  }
  // Same breathing room between header and content as the legacy screens.
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
}

void OpdsBookBrowserActivity::buildBrowsingScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, true);

  const size_t count = knownRows();
  if (count == 0) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }

  // Rows are formatted on demand for the visible viewport only, so a redraw
  // never allocates; after a download the heap can be too fragmented for it.
  fui::ListProps props;
  props.rowProvider = &OpdsBookBrowserActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(count);
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.valueInset = 8;               // air between the nav chevron and the row edge
  props.iconsInMargin = true;         // the check sits left of the title: nothing else moves
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(count));  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void OpdsBookBrowserActivity::provideRow(void* user, const uint16_t index, fui::ListItem& item) {
  auto& self = *static_cast<OpdsBookBrowserActivity*>(user);
  const OpdsEntry* row = self.rowEntry(index);
  if (!row) {
    // Outside the window (a far scroll): keepWindowAroundView() loads the
    // page and redraws; the row shows blank meanwhile.
    item.label = "";
    item.actionValue = static_cast<int16_t>(index);
    return;
  }
  const OpdsEntry& entry = *row;
  const size_t slot = index - self.windowBase;
  item.label = entry.title.c_str();
  if (entry.type == OpdsEntryType::BOOK && !entry.author.empty()) item.subtitle = entry.author.c_str();
  // 20 px bold check in the row inset left of the title (Lyra: 20 inset + 8
  // padding); the list clamps it to that room, so titles never move.
  // One mark for "no network needed": a downloaded book or a cached feed page.
  if ((entry.type == OpdsEntryType::BOOK && self.onSd[slot]) || self.pageCached[slot]) {
    item.icon = fui::bitmapFromIcon(icon_check_20);
  }
  if (entry.type == OpdsEntryType::NAVIGATION) {
    if (entry.count >= 0) {
      // Right-aligned "(<count>) >"; the list reads it before the next provider call.
      snprintf(self.countLabel.data(), self.countLabel.size(), "(%ld) >", static_cast<long>(entry.count));
      item.value = self.countLabel.data();
    } else {
      item.value = ">";
    }
  }
  item.actionValue = static_cast<int16_t>(index);
}

void OpdsBookBrowserActivity::buildDownloadScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);

  // Centered block: status line, book title, progress bar, cancel button.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const int16_t barH = 16;
  const int16_t btnH = theme.rowHeight;
  const int16_t blockH = static_cast<int16_t>(lh * 3 + barH + btnH + gap * 4);
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), downloadReceiving ? tr(STR_DOWNLOADING) : tr(STR_CONNECTING), centered);
  screen.target().text(screen.takeTop(lh, gap), statusMessage.c_str(), centered);

  const fui::Rect bar = screen.takeTop(barH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (downloadTotal > 0) {
    fui::ProgressBarProps progress;
    progress.value = static_cast<int32_t>(downloadProgress);
    progress.max = static_cast<int32_t>(downloadTotal);
    progress.border = fui::Paint::solid(fui::Color::Black);
    progress.borderWidth = 1;
    fui::progressBar(screen.frame(), bar, progress);
  }
  // "12.3 / 33.0 MB": shows whether a stalled bar is still moving.
  char sizeLine[40] = "";
  if (downloadReceiving) {
    char done[16];
    formatFileSize(downloadProgress, done, sizeof(done));
    if (downloadTotal > 0) {
      char total[16];
      formatFileSize(downloadTotal, total, sizeof(total));
      snprintf(sizeLine, sizeof(sizeLine), "%s / %s", done, total);
    } else {
      snprintf(sizeLine, sizeof(sizeLine), "%s", done);
    }
  }
  screen.target().text(screen.takeTop(lh, gap), sizeLine, centered);

  const fui::Rect btnArea = screen.takeTop(btnH);
  const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
  fui::ButtonProps cancel;
  cancel.label = tr(STR_CANCEL);
  cancel.action = ACTION_CANCEL;
  screen.button(cancel, fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
}

void OpdsBookBrowserActivity::buildStatusScreen(UiApp::ScreenType& screen) {
  screenHeader(screen, false);

  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  if (state == BrowserState::ERROR) {
    const int16_t lh = screen.target().lineHeight(centered.font);
    const int16_t gap = screen.theme().spaceMd;
    const bool showTapHint = mappedInput.hasTouch();
    const int16_t blockH = static_cast<int16_t>(lh * (showTapHint ? 3 : 2) + gap * (showTapHint ? 2 : 1));
    const fui::Rect body = screen.body();
    if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
    screen.target().text(screen.takeTop(lh, gap), tr(STR_ERROR_MSG), centered);
    screen.target().text(screen.takeTop(lh, gap), errorMessage.c_str(), centered);
    if (showTapHint) screen.target().text(screen.takeTop(lh), tr(STR_TAP_TO_RETRY), centered);
    return;
  }
  // CHECK_WIFI / LOADING (and the brief child-activity handoff states).
  screen.centeredText(statusMessage.c_str(), centered);
}

void OpdsBookBrowserActivity::render(RenderLock&&) {
  renderer.clearScreen();

  MappedInputManager::Labels labels;
  switch (state) {
    case BrowserState::BROWSING: {
      const OpdsEntry* selected = rowEntry(selectorIndex);
      const char* confirmLabel = (selected && selected->type == OpdsEntryType::BOOK) ? tr(STR_DOWNLOAD) : tr(STR_OPEN);
      const char* searchLabel = (hasSearch() && selectorIndex == 0) ? tr(STR_SEARCH) : tr(STR_DIR_UP);
      labels =
          mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), confirmLabel, searchLabel, tr(STR_DIR_DOWN));
      break;
    }
    case BrowserState::DOWNLOADING:
      labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      break;
    case BrowserState::ERROR:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_RETRY), "", "");
      break;
    default:
      labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
      break;
  }
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  uiReady = false;
  renderUiApp(app, uiTarget);
  uiReady = true;
  if (state == BrowserState::DOWNLOADING) {
    const ShownDownloadFrame frame{true, downloadReceiving, downloadProgress, downloadTotal};
    const bool same = shownDownloadFrame.valid && frame.receiving == shownDownloadFrame.receiving &&
                      frame.progress == shownDownloadFrame.progress && frame.total == shownDownloadFrame.total;
    shownDownloadFrame = frame;
    if (same) {
      LOG_DBG("OPDS", "Download frame unchanged, refresh skipped");
      return;
    }
  } else {
    shownDownloadFrame.valid = false;
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}

void OpdsBookBrowserActivity::showLoadingBeforeFetch(const std::string& path) {
  // A cached page parses in tens of ms, so a Loading frame would only add one
  // e-ink refresh (~600 ms) ahead of the list.
  if (pageCache) {
    if (preload) preload->collect();
    const std::string url = UrlUtils::buildUrl(server.url, path);
    if (pageCache->contains(url)) {
      LOG_INF("OPDS", "Cache hit, no Loading frame: %s", UrlUtils::maskUserInfo(url).c_str());
      return;
    }
    // The tapped page goes first: other downloads stop sharing Wi-Fi with it
    // while the Loading frame draws, not after. Its own download starts now
    // (on a free worker) so the handshake overlaps the frame's refresh;
    // fetchFeed() then streams it, or fetches in the foreground if no worker
    // took it.
    if (preload) {
      preload->cancelOthers(url);
      preload->enqueue(url, true);
      preload->pump();
    }
  }
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
    LOG_ERR("OPDS", "Loading screen could not be rendered before feed fetch");
    requestUpdate(true);
  }
  if (preload) preload->pump();  // a worker cancelled above may have freed up meanwhile
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path, const int restoreRow, const int restoreTop,
                                        const bool recheck, const bool stream) {
  if (!ensureEntryBuffer()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }

#ifdef SIMULATOR
  clearEntries();
  searchTemplate = "simulator://search?query={searchTerms}";

  if (path.empty()) {
    appendEntry(OpdsEntry{OpdsEntryType::NAVIGATION, "Browse fiction", "", "/fiction", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "The Left Hand of Darkness", "Ursula K. Le Guin",
                          "/books/the-left-hand-of-darkness.epub", ""});
    appendEntry(
        OpdsEntry{OpdsEntryType::BOOK, "A Room of One's Own", "Virginia Woolf", "/books/a-room-of-ones-own.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "Frankenstein", "Mary Shelley", "/books/frankenstein.epub", ""});
  } else {
    appendEntry(
        OpdsEntry{OpdsEntryType::BOOK, "The Dispossessed", "Ursula K. Le Guin", "/books/the-dispossessed.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "Kindred", "Octavia E. Butler", "/books/kindred.epub", ""});
    appendEntry(OpdsEntry{OpdsEntryType::BOOK, "The Time Machine", "H. G. Wells", "/books/the-time-machine.epub", ""});
  }
  pages.push_back(PageInfo{PsramString(path), 0, static_cast<uint16_t>(entryCount)});

  selectorIndex = 0;
  topIndex = 0;
  state = BrowserState::BROWSING;
  requestUpdate();
  return;
#endif

  if (server.url.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_NO_SERVER_URL);
    requestUpdate();
    return;
  }

  clearEntries();
  const std::string url = UrlUtils::buildUrl(server.url, path);
  loadPage(url, path, 0, restoreRow, restoreTop, recheck, stream);
  if (streaming() || state != BrowserState::BROWSING) return;
  // Back: the saved row may sit on a later page; pull cached pages in until
  // it is on the list (an uncached one would block: the user scrolls to it).
  while (restoreRow >= static_cast<int>(knownRows()) && !nextPagePath.empty() && pageCache &&
         pageCache->contains(UrlUtils::buildUrl(server.url, nextPagePath))) {
    appendNextPage(false);
  }
  selectorIndex = std::min(std::max(restoreRow, 0), static_cast<int>(knownRows()) - 1);
  topIndex = restoreTop;
}

void OpdsBookBrowserActivity::appendNextPage(const bool stream) {
  if (nextPagePath.empty() || streaming()) return;
  // A new page's size is unknown: keep a quarter of the window free for it.
  if (makeRoom(OPDS_BROWSER_ENTRY_CAPACITY / 4, true) == 0) return;
  const std::string path = std::move(nextPagePath);
  nextPagePath.clear();  // finishFeed() sets the page after this one
  loadPage(UrlUtils::buildUrl(server.url, path), path, entryCount, 0, 0, false, stream);
}

size_t OpdsBookBrowserActivity::knownRows() const {
  if (pages.empty()) return 0;
  return std::min<size_t>(pages.back().firstRow + pages.back().rows, UINT16_MAX);
}

const OpdsEntry* OpdsBookBrowserActivity::rowEntry(const int row) const {
  if (!entries || row < static_cast<int>(windowBase) || row >= static_cast<int>(windowBase + entryCount))
    return nullptr;
  return &entries[row - windowBase];
}

const OpdsBookBrowserActivity::PageInfo* OpdsBookBrowserActivity::pageStartingAt(const uint32_t row) const {
  const auto page = std::find_if(pages.begin(), pages.end(), [row](const PageInfo& p) { return p.firstRow == row; });
  return page == pages.end() ? nullptr : &*page;
}

const OpdsBookBrowserActivity::PageInfo* OpdsBookBrowserActivity::pageEndingAt(const uint32_t row) const {
  const auto page =
      std::find_if(pages.begin(), pages.end(), [row](const PageInfo& p) { return p.firstRow + p.rows == row; });
  return page == pages.end() ? nullptr : &*page;
}

void OpdsBookBrowserActivity::shiftWindow(const int delta, const size_t fromRow) {
  if (delta > 0) {
    for (size_t i = entryCount; i > fromRow; --i) entries[i - 1 + delta] = std::move(entries[i - 1]);
    for (size_t i = fromRow; i < fromRow + delta; ++i) entries[i] = OpdsEntry{};
    onSd <<= delta;
    pageCached <<= delta;
    entryCount += delta;
    windowBase -= delta;
  } else if (delta < 0) {
    const size_t drop = static_cast<size_t>(-delta);
    for (size_t i = drop; i < entryCount; ++i) entries[i - drop] = std::move(entries[i]);
    for (size_t i = entryCount - drop; i < entryCount; ++i) entries[i] = OpdsEntry{};
    onSd >>= drop;
    pageCached >>= drop;
    entryCount -= drop;
    windowBase += drop;
  }
}

size_t OpdsBookBrowserActivity::makeRoom(const size_t rows, const bool atFront) {
  while (entryCount + rows > OPDS_BROWSER_ENTRY_CAPACITY) {
    const PageInfo* page = atFront ? pageStartingAt(windowBase) : pageEndingAt(windowBase + entryCount);
    if (!page || page->rows == 0 || page->rows >= entryCount) break;
    // Never drop a page the view (or the selection) is on.
    const int viewTop = std::min(topIndex, selectorIndex);
    const int viewEnd = std::max(topIndex + visibleRows, selectorIndex + 1);
    const bool inView =
        static_cast<int>(page->firstRow)<viewEnd&& static_cast<int>(page->firstRow + page->rows)> viewTop;
    if (inView) break;
    RenderLock lock(*this);
    if (atFront) {
      shiftWindow(-static_cast<int>(page->rows), 0);
    } else {
      const size_t keep = entryCount - page->rows;
      for (size_t i = keep; i < entryCount; ++i) entries[i] = OpdsEntry{};
      entryCount = keep;
    }
    LOG_DBG("OPDS", "Window %s dropped %u rows: rows %zu-%zu", atFront ? "front" : "back",
            static_cast<unsigned>(page->rows), windowBase, windowBase + entryCount);
  }
  return OPDS_BROWSER_ENTRY_CAPACITY - entryCount;
}

void OpdsBookBrowserActivity::reloadPage(const PageInfo& page, const bool front) {
  if (makeRoom(page.rows, !front) < page.rows) return;
  const std::string path(page.path.c_str());
  const std::string url = UrlUtils::buildUrl(server.url, path);
  size_t slot = entryCount;
  {
    RenderLock lock(*this);
    if (front) {
      shiftWindow(static_cast<int>(page.rows), 0);
      slot = 0;
    } else {
      entryCount += page.rows;
    }
  }
  // Blocking: a cached page parses in tens of ms; one evicted from the cache
  // (or changed its row count) fetches again, and any rows it no longer
  // fills stay blank.
  OpdsParser parser(entries.get() + slot, page.rows);
  if (!loadFeed(url, parser) || !parser) LOG_ERR("OPDS", "Page reload failed: %s", UrlUtils::maskUserInfo(url).c_str());
  {
    RenderLock lock(*this);
    for (size_t i = slot; i < slot + page.rows; ++i) {
      if (entries[i].type == OpdsEntryType::NAVIGATION) replaceFolderEmoji(entries[i].title);
    }
  }
  markBooksOnSd(slot, slot + page.rows);
  markCachedFeeds();
  const int viewEnd = topIndex + visibleRows;
  if (static_cast<int>(windowBase + slot) < viewEnd && static_cast<int>(windowBase + slot + page.rows) > topIndex) {
    requestUpdate();
  }
  LOG_INF("OPDS", "Window reloaded rows %zu-%zu (%s)", windowBase + slot, windowBase + slot + page.rows,
          front ? "front" : "back");
  if (preload) preload->pump();
}

void OpdsBookBrowserActivity::keepWindowAroundView() {
  const int ahead = APPEND_AHEAD_SCREENS * visibleRows;
  const int viewEnd = topIndex + visibleRows;
  const int windowEnd = static_cast<int>(windowBase + entryCount);
  if (viewEnd + ahead > windowEnd) {
    // Forward: a page seen before re-parses from the cache; past the last
    // known page the feed's next page is fetched (streamed when uncached).
    if (const PageInfo* next = pageStartingAt(windowEnd)) {
      reloadPage(*next, false);
    } else if (!nextPagePath.empty() && (appendRetryMs == 0 || millis() - appendRetryMs >= APPEND_RETRY_MS)) {
      appendNextPage(true);
    }
    return;
  }
  if (windowBase > 0 && topIndex - ahead < static_cast<int>(windowBase)) {
    if (const PageInfo* previous = pageEndingAt(windowBase)) reloadPage(*previous, true);
  }
}

void OpdsBookBrowserActivity::loadPage(const std::string& url, const std::string& pagePath, const size_t base,
                                       const int restoreRow, const int restoreTop, const bool recheck,
                                       const bool stream) {
  if (stream && startStream(url, pagePath, base, restoreRow, restoreTop, recheck)) return;
  OpdsParser parser(entries.get() + base, OPDS_BROWSER_ENTRY_CAPACITY - base);
  fetchCancelled = false;
  OpdsFeedError feedError = OpdsFeedError::Other;
  if (!loadFeed(url, parser, &feedError)) {
    if (base > 0) {
      // The list stays as it is; the append is retried after a pause.
      LOG_ERR("OPDS", "Next page failed (%d): %s", static_cast<int>(feedError), UrlUtils::maskUserInfo(url).c_str());
      nextPagePath = pagePath;
      appendRetryMs = millis() | 1;
      return;
    }
    if (fetchCancelled) {
      // pollFetchCancel() consumed the Back press, so act on it here.
      LOG_INF("OPDS", "Feed fetch cancelled by Back");
      fetchCancelled = false;
      navigateBack();
      return;
    }
    state = BrowserState::ERROR;
    switch (feedError) {
      case OpdsFeedError::Unauthorized:
        errorMessage = tr(STR_FEED_UNAUTHORIZED);
        break;
      case OpdsFeedError::Redirected:
        errorMessage = tr(STR_FEED_REDIRECTED);
        break;
      case OpdsFeedError::ServerError:
        errorMessage = tr(STR_FEED_SERVER_ERROR);
        break;
      case OpdsFeedError::Unreachable:
        errorMessage = tr(STR_FEED_UNREACHABLE);
        break;
      case OpdsFeedError::Other:
        errorMessage = tr(STR_FETCH_FEED_FAILED);
        break;
    }
    requestUpdate();
    return;
  }

  if (!parser) {
    // Never keep bytes that do not parse; a retry must go back to the server.
    if (pageCache) pageCache->erase(url);
    if (base > 0) {
      LOG_ERR("OPDS", "Next page does not parse: %s", UrlUtils::maskUserInfo(url).c_str());
      return;  // the list ends here
    }
    state = BrowserState::ERROR;
    errorMessage = parser.getErrorReason() == OpdsParserError::BUFFER_MEMORY ? tr(STR_OPDS_FEED_BUFFER_MEMORY_ERROR)
                                                                             : tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }

  finishFeed(parser, url, pagePath, base, restoreRow, restoreTop, recheck);
}

void OpdsBookBrowserActivity::finishFeed(const OpdsParser& parser, const std::string& url, const std::string& pagePath,
                                         const size_t base, const int restoreRow, const int restoreTop,
                                         const bool recheck) {
  if (base == 0) {
    searchTemplate = parser.getSearchTemplate();
    searchDescriptionUrl = parser.getSearchDescriptionUrl();
    feedTotal = parser.getTotalResults();
  }
  // The list may be on screen already (a streamed page, an appended one): the
  // render task reads it, and only what changes in view (rows, marks) redraws.
  const bool shown = state == BrowserState::BROWSING && (base > 0 || entryCount > 0);
  const size_t shownCount = shown ? entryCount : base;
  RenderLock lock(*this);
  entryCount = base + parser.getEntryCount();
  for (size_t i = base; i < entryCount; ++i) {
    if (entries[i].type == OpdsEntryType::NAVIGATION) replaceFolderEmoji(entries[i].title);
  }
  if (!shown) {
    // The feed may have changed since the row was saved: clamp the row here and
    // leave topIndex to render()'s clamp.
    selectorIndex = entryCount > 0 ? std::min(std::max(restoreRow, 0), static_cast<int>(entryCount) - 1) : 0;
    topIndex = restoreTop;
    if (restoreRow != 0 || restoreTop != 0) LOG_DBG("OPDS", "Restored row %d top %d", selectorIndex, topIndex);
  }
  lock.unlock();

  // A page joins the directory once; the page after it, resolved against it
  // as navigateToEntry() resolves rows, so its cache key matches the
  // prefetch below.
  const std::string& nextHref = parser.getNextPageUrl();
  nextPagePath.clear();
  appendRetryMs = 0;
  pages.push_back(PageInfo{PsramString(pagePath.c_str()), static_cast<uint32_t>(windowBase + base),
                           static_cast<uint16_t>(parser.getEntryCount())});
  if (parser.wasTruncated() || knownRows() >= UINT16_MAX) {
    LOG_INF("OPDS", "List full at %zu rows, later pages are not loaded", knownRows());
  } else if (base > 0 && parser.getEntryCount() == 0) {
    LOG_INF("OPDS", "Empty page ends the list");  // never append in a loop
  } else if (!nextHref.empty()) {
    nextPagePath = UrlUtils::buildUrl(UrlUtils::buildUrl(server.url, pagePath), nextHref);
  }

  const auto onSdBefore = onSd;
  markBooksOnSd(shownCount);  // rows shown earlier keep their marks
  const bool marksChanged = markCachedFeeds() || onSd != onSdBefore;
  marksPendingMs = 0;  // the redraw below shows them
  state = entryCount == 0 ? BrowserState::ERROR : BrowserState::BROWSING;
  if (entryCount == 0) {
    // Kept cached (the server list marks it fetched); Retry refetches it.
    errorMessage = tr(STR_NO_ENTRIES);
  }
  const bool rowsInView =
      entryCount > shownCount && windowBase + shownCount < static_cast<size_t>(topIndex + visibleRows);
  if (!shown || rowsInView || marksChanged) requestUpdate();

  if (preload) {
    // The next page downloads now so it is in the cache before the view
    // nears the end of the list.
    if (!nextPagePath.empty()) preload->enqueue(UrlUtils::buildUrl(server.url, nextPagePath), true);
    if (base == 0 && currentPath.empty()) preloadFeedsOnPage();
    if (base == 0 && recheck && shownFromCache) preload->revalidate(url);
    preload->pump();
  }
}

bool OpdsBookBrowserActivity::loadFeed(const std::string& url, OpdsParser& parser, OpdsFeedError* error) {
  // A cache hit needs no network, so background downloads keep going.
  // Otherwise a foreground request never overlaps them: finish the one for
  // this very page, pause the rest. Either way completed pages land in the
  // cache.
  shownFromCache = false;
  if (pageCache) {
    const OpdsPageBuffer* cached = pageCache->find(url);
    shownFromCache = cached != nullptr;
    if (!cached && preload) {
      const unsigned long joinStart = millis();
      if (preload->pause(url)) LOG_INF("OPDS", "Waited %lu ms for background fetch", millis() - joinStart);
      cached = pageCache->find(url);
    }
    if (cached) {
      LOG_INF("OPDS", "Cached: %s (%zu bytes)", UrlUtils::maskUserInfo(url).c_str(), cached->size());
      parser.parse(cached->data(), cached->size());
      return true;
    }
  }

  // Keep the normalized server URL alive for the synchronous fetch so
  // HttpDownloader can scope Basic auth even for legacy scheme-less entries.
  const std::string authorizationOrigin = UrlUtils::ensureProtocol(server.url);
  LOG_DBG("OPDS", "Fetching: %s", UrlUtils::maskUserInfo(url).c_str());
  // Tee the response into PSRAM so this page is cached before the user moves
  // on; the parser still consumes it as it streams.
  OpdsPageBuffer page(MemoryPool::Psram, OPDS_PAGE_MAX_BYTES);
  const bool cachePage = pageCache != nullptr;
  {
    OpdsParserStream parserStream{parser};
    HttpDownloader::DownloadOptions downloadOptions;
    downloadOptions.transport = HttpDownloader::Transport::WOLFSSL;
    downloadOptions.authorizationOrigin = authorizationOrigin;
    downloadOptions.connection = feedConnectionForRequest();
    downloadOptions.shouldCancel = [this]() { return pollFetchCancel(); };
    HttpRequestDiagnostics diagnostics;
    const auto result = HttpDownloader::streamUrl(
        url,
        [&parserStream, &page, &parser, cachePage](const uint8_t* data, const size_t len) {
          if (cachePage) page.append(data, len);  // overflow only skips caching
          // Stop at the entry limit: the rest of a huge feed is never shown.
          return parserStream.write(data, len) == len && !parser.wasTruncated();
        },
        nullptr, server.username, server.password, std::move(downloadOptions), &diagnostics);
    if (parser.wasTruncated() && !fetchCancelled) {
      LOG_INF("OPDS", "Stopped feed at entry limit after %zu bytes", page.size());
    } else if (result != HttpDownloader::OK) {
      if (error) *error = classifyFeedFailure(diagnostics, result == HttpDownloader::HTTP_ERROR);
      return false;
    }
  }

  if (cachePage && parser && !page.failed()) pageCache->store(url, std::move(page), true, millis());
  return true;
}

// Runs on this task while loadFeed() blocks the loop: Back is the only way
// out of a slow or dead request, as in the LOADING state of loop().
bool OpdsBookBrowserActivity::pollFetchCancel() {
  if (fetchCancelled) return true;
  mappedInput.update();
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    fetchCancelled = true;
  }
  return fetchCancelled;
}

void OpdsBookBrowserActivity::preloadFeedsOnPage() {
  if (!preload) return;
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  // Skips books, whose links are downloads rather than feeds.
  size_t queuedCount = 0;
  for (size_t i = 0; i < entryCount; ++i) {
    if (entries[i].type != OpdsEntryType::NAVIGATION || entries[i].href.empty()) continue;
    const std::string path = UrlUtils::buildUrl(feedUrl, std::string(entries[i].href));
    preload->enqueue(UrlUtils::buildUrl(server.url, path), false);
    ++queuedCount;
  }
  LOG_INF("OPDS", "Preload queued %zu feeds from the first page", queuedCount);
}

freeink::SecureHttpClient* OpdsBookBrowserActivity::feedConnectionForRequest() {
#if defined(FREEINK_NET_WOLFSSL)
  const unsigned long now = millis();
  if (feedConnection && now - feedConnectionLastUseMs > OPDS_KEEPALIVE_MAX_IDLE_MS) {
    LOG_DBG("OPDS", "Closing feed connection idle %lu ms", now - feedConnectionLastUseMs);
    feedConnection->end();
  }
  // Small object (no buffers until it connects); on failure every request
  // just opens its own connection as before.
  if (!feedConnection) feedConnection = makeUniqueNoThrow<freeink::SecureHttpClient>();
  feedConnectionLastUseMs = now;
  return feedConnection.get();
#else
  return nullptr;
#endif
}

void OpdsBookBrowserActivity::stopPrefetch() {
  if (!preload) return;
  preload->pause("");
  preload->closeConnections();
}

bool OpdsBookBrowserActivity::startStream(const std::string& url, const std::string& path, const size_t base,
                                          const int restoreRow, const int restoreTop, const bool recheck) {
  if (!preload || !pageCache || pageCache->contains(url)) return false;
  // showLoadingBeforeFetch() queued the page; a pump here covers the callers
  // that did not (the first page after a Wi-Fi join) and a worker freed since.
  preload->enqueue(url, true);
  preload->pump();
  if (!preload->fetching(url)) return false;  // no free worker: fetch in the foreground
  stream.parser = makeUniqueNoThrow<OpdsParser>(entries.get() + base, OPDS_BROWSER_ENTRY_CAPACITY - base);
  if (!stream.parser || stream.parser->error()) {
    LOG_ERR("OPDS", "No parser for the streamed feed");
    stream.parser.reset();
    return false;
  }
  stream.url = url;
  stream.path = path;
  stream.base = base;
  stream.drained = 0;
  stream.restoreRow = restoreRow;
  stream.restoreTop = restoreTop;
  stream.recheck = recheck;
  shownFromCache = false;
  LOG_INF("OPDS", "Streaming: %s", UrlUtils::maskUserInfo(url).c_str());
  return true;
}

void OpdsBookBrowserActivity::pumpStream() {
  OpdsPagePrefetcher* job = preload->fetching(stream.url);
  if (!job) {
    // The worker dropped the page (network failure, oversized) or pump()
    // cached it first: the foreground fetch retries once (a cache hit, or
    // the error named as before).
    LOG_INF("OPDS", "Streamed page gone, fetching in the foreground");
    const StreamFetch retry = std::move(stream);
    abortStream();
    loadPage(retry.url, retry.path, retry.base, retry.restoreRow, retry.restoreTop, retry.recheck, false);
    return;
  }
  const bool done = !job->running();
  job->drain(stream.drained, *stream.parser);
  OpdsParser& parser = *stream.parser;
  if (done) parser.flush();
  if (!parser) {
    // Never keep bytes that do not parse: abortStream() cancels the worker,
    // which then drops them.
    const bool memory = parser.getErrorReason() == OpdsParserError::BUFFER_MEMORY;
    const std::string url = stream.url;
    const bool append = stream.base > 0;
    abortStream();
    preload->collect();     // a finished page would be cached by the next pump()
    pageCache->erase(url);  // so a retry goes back to the server
    if (append) {
      LOG_ERR("OPDS", "Next page does not parse: %s", UrlUtils::maskUserInfo(url).c_str());
      {
        RenderLock rows(*this);  // drop the partial rows shown so far
        for (size_t i = stream.base; i < entryCount; ++i) entries[i] = OpdsEntry{};
        entryCount = stream.base;
      }
      return;  // the list ends here
    }
    clearEntries();
    state = BrowserState::ERROR;
    errorMessage = memory ? tr(STR_OPDS_FEED_BUFFER_MEMORY_ERROR) : tr(STR_PARSE_FEED_FAILED);
    requestUpdate();
    return;
  }
  publishStreamedRows(done);
  if (!done) return;
  preload->collect();  // the whole page lands in the cache
  const std::unique_ptr<OpdsParser> finished = std::move(stream.parser);
  const StreamFetch ended = std::move(stream);
  abortStream();
  finishFeed(*finished, ended.url, ended.path, ended.base, ended.restoreRow, ended.restoreTop, ended.recheck);
}

void OpdsBookBrowserActivity::publishStreamedRows(const bool done) {
  const size_t parsed = stream.base + stream.parser->getEntryCount();
  if (parsed <= entryCount) return;
  const size_t first = entryCount;
  {
    RenderLock lock(*this);
    for (size_t i = first; i < parsed; ++i) {
      if (entries[i].type == OpdsEntryType::NAVIGATION) replaceFolderEmoji(entries[i].title);
    }
    entryCount = parsed;
  }
  if (state == BrowserState::LOADING) {
    // The Loading frame stays until a screenful is in: fewer refreshes.
    // shortcut: before the first list render visibleRows is still 1, so a
    // typical row count stands in; measure it from the theme if that shows
    // too few rows first.
    const int screenful = visibleRows > 1 ? visibleRows : 10;
    if (!done && parsed < static_cast<size_t>(screenful)) return;
    selectorIndex = std::min(std::max(stream.restoreRow, 0), static_cast<int>(entryCount) - 1);
    topIndex = stream.restoreTop;
    state = BrowserState::BROWSING;
    LOG_INF("OPDS", "Showing %zu rows while the feed streams", entryCount);
  } else if (windowBase + first >= static_cast<size_t>(topIndex + visibleRows)) {
    return;  // the new rows are below the viewport: no redraw
  }
  markBooksOnSd(first);
  markCachedFeeds();
  requestUpdate();
}

void OpdsBookBrowserActivity::abortStream() {
  if (!streaming()) return;
  if (preload) {  // already gone on exit, with its workers joined
    if (OpdsPagePrefetcher* job = preload->fetching(stream.url); job && job->running()) job->cancel();
  }
  stream.parser.reset();
}

// Feed rows resolved to cache keys as navigateToEntry()
// and the preloads do. Main loop only: the cache has no lock.
bool OpdsBookBrowserActivity::markCachedFeeds() {
  std::bitset<ENTRY_CAPACITY> cached;
  if (pageCache) {
    pageCachedAt = pageCache->changes();
    const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
    for (size_t i = 0; i < entryCount; ++i) {
      if (entries[i].type != OpdsEntryType::NAVIGATION || entries[i].href.empty()) continue;
      const std::string path = UrlUtils::buildUrl(feedUrl, std::string(entries[i].href));
      if (pageCache->contains(UrlUtils::buildUrl(server.url, path))) cached.set(i);
    }
  }
  if (cached == pageCached) return false;
  RenderLock lock(*this);
  pageCached = cached;
  return true;
}

// One pass over the download folder, not an exists() per book: each lookup
// rescans the folder, so 50 books would read it 50 times. Read-only.
void OpdsBookBrowserActivity::markBooksOnSd(const size_t from, size_t to) {
  to = std::min(to, entryCount);
  for (size_t i = from; i < to; ++i) onSd.reset(i);
  // cppcheck-suppress unreadVariable ; read only by LOG_DBG, compiled out in release
  const unsigned long startMs = millis();
  const std::string folder = downloadFolderFor(server);
  if (folder.empty()) return;
  // Transient: the new rows' expected file names, freed on return (PSRAM:
  // thousands of rows would split internal RAM). With server filenames only
  // the generated fallback name can be predicted.
  std::vector<PsramString, PsramPreferAllocator<PsramString>> names(to);
  size_t wanted = 0;
  for (size_t i = from; i < to; ++i) {
    if (entries[i].type != OpdsEntryType::BOOK) continue;
    const std::string path = bookDownloadPath(entries[i], server.filenameFormat, folder);
    names[i].assign(path, path.rfind('/') + 1, std::string::npos);
    // Library index first: the same book in any folder, by metadata or by name.
    if (!libraryKeys.empty()) {
      const uint64_t keys[2] = {libraryTitleKey(std::string(entries[i].title), std::string(entries[i].author)),
                                libraryNameKey(std::string(names[i]))};
      for (const uint64_t key : keys) {
        if (std::binary_search(libraryKeys.begin(), libraryKeys.end(), key)) {
          onSd.set(i);
          break;
        }
      }
    }
    if (onSd[i]) continue;
    ++wanted;
  }
  if (wanted == 0) return;
  HalFile dir = Storage.open(folder.c_str());
  if (dir && dir.isDirectory()) {
    char name[256];
    size_t found = 0;
    for (HalFile file = dir.openNextFile(); file && found < wanted; file = dir.openNextFile()) {
      file.getName(name, sizeof(name));
      const bool isFile = !file.isDirectory();
      file.close();
      if (!isFile) continue;
      for (size_t i = from; i < to; ++i) {
        if (!onSd[i] && !names[i].empty() && strcasecmp(name, names[i].c_str()) == 0) {
          onSd.set(i);
          ++found;
        }
      }
    }
  }
  if (dir) dir.close();
  LOG_DBG("OPDS", "On SD: %u of %u books (%lu ms)", static_cast<unsigned>(onSd.count()), static_cast<unsigned>(wanted),
          millis() - startMs);
}

// One pass over the Library index on entry (2000 books: 256 KB of records, read
// from a PSRAM copy) into 16 bytes per book, so marking a page costs no I/O.
// The index can lag the card until the Library next rebuilds it, which is why
// the download-folder scan stays.
void OpdsBookBrowserActivity::loadLibraryKeys() {
  libraryKeys.clear();
  library::LibraryIndexFile index;
  if (!index.open(library::libraryIndexPath())) return;  // no index yet: folder scan only
  index.loadIntoMemory();                                // PSRAM copy; on failure records come off the card
  const uint16_t count = index.bookCount();
  libraryKeys.reserve(static_cast<size_t>(count) * 2);
  library::ClixRecord record;
  std::string name;
  for (uint16_t i = 0; i < count; ++i) {
    if (!index.readRecord(i, record)) break;
    std::string key(record.fold, record.foldLen);
    key += '\0';
    key.append(record.authorKey, record.authorKeyLen);
    libraryKeys.push_back(library::clixPathHash(key.data(), key.size()));
    if (index.readName(record, name)) libraryKeys.push_back(libraryNameKey(name));
  }
  index.close();
  std::sort(libraryKeys.begin(), libraryKeys.end());
  LOG_DBG("OPDS", "Library keys: %u books", static_cast<unsigned>(count));
}

bool OpdsBookBrowserActivity::ensureEntryBuffer() {
  if (entries) return true;
  entries = makeUniqueNoThrow<OpdsEntry[]>(OPDS_BROWSER_ENTRY_CAPACITY);
  return entries != nullptr;
}

void OpdsBookBrowserActivity::clearEntries() {
  abortStream();
  // The app's interaction table still references the old row indices until
  // the next render, so stop routing touches while clearing the backing data.
  uiReady = false;
  for (size_t i = 0; entries && i < entryCount; ++i) {
    entries[i] = OpdsEntry{};
  }
  entryCount = 0;
  windowBase = 0;
  pages.clear();
  feedTotal = -1;
  nextPagePath.clear();
  appendRetryMs = 0;
}

bool OpdsBookBrowserActivity::appendEntry(OpdsEntry&& entry) {
  if (!entries || entryCount >= OPDS_BROWSER_ENTRY_CAPACITY) return false;
  entries[entryCount++] = std::move(entry);
  return true;
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  pushHistory();
  // Resolve to a full URL so sub-sub-navigation retains parent path context
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  currentPath = UrlUtils::buildUrl(feedUrl, std::string(entry.href));

  clearEntries();
  selectorIndex = 0;
  showLoadingBeforeFetch(currentPath);
  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::leaveBrowser() {
  if (fromServerList) {
    leavingToList = true;
#ifndef SIMULATOR
    keepWifiForNextScreen();  // the list finds Wi-Fi up and shows its check marks at once
#endif
    activityManager.goToBrowser();
  } else {
    onGoHome();
  }
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    leaveBrowser();
  } else {
    const HistoryEntry previous = std::move(navigationHistory.back());
    navigationHistory.pop_back();
    currentPath = previous.path;
    clearEntries();
    selectorIndex = 0;
    showLoadingBeforeFetch(currentPath);
    // Back lands on the row that was opened, scrolled as it was.
    fetchFeed(currentPath, previous.selectorIndex, previous.topIndex);
  }
}

void OpdsBookBrowserActivity::requestDownload(const OpdsEntry& book) {
  // One prompt before every download: "Download? <title>", or the overwrite
  // question with the copy's size and date when the book is already on SD.
  // Read-only open: one directory lookup, no SD write until Confirm.
  const std::string folder = downloadFolderFor(server);
  if (folder.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_OPDS_INVALID_FOLDER);
    requestUpdate();
    return;
  }
  std::string path = bookDownloadPath(book, server.filenameFormat, folder);
  // The server's name is only known once the download starts; a clash then
  // gets its own overwrite prompt (offerOverwrite).
  const bool serverFilename = server.filenameFormat == OpdsFilenameFormat::SERVER_FILENAME;
  std::string heading = tr(STR_CONFIRM_DOWNLOAD_PROMPT);
  // cppcheck-suppress stlcstrAssignment ; title is a PsramString, not std::string
  std::string details = book.title.c_str();
  HalFile existing;
  if (!serverFilename) existing = Storage.open(path.c_str());
  const bool onSdAlready = existing && !existing.isDirectory();
  if (onSdAlready) {
    char sizeLabel[16];
    formatFileSize(existing.fileSize64(), sizeLabel, sizeof(sizeLabel));
    char dateLabel[20];
    const bool hasDate = formatFatDateTime(existing.modificationTime(), dateLabel, sizeof(dateLabel));
    heading = tr(STR_BOOK_EXISTS_OVERWRITE);
    details = sizeLabel;
    if (hasDate) {
      details += ", ";
      details += dateLabel;
    }
    LOG_INF("OPDS", "Already on SD: %s (%s)", path.c_str(), details.c_str());
  }
  if (existing) existing.close();

  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, details);
  if (!dialog) {
    LOG_ERR("OPDS", "Cannot allocate download dialog");
    return;
  }
  // Download size: the feed's length attribute, else a background request
  // (bookDownloader is idle while browsing) that fills it in; Confirm works
  // throughout. "?" when neither tells. Not for a book already on SD: the
  // overwrite question shows the copy's size, and no request is made.
  char size[16] = "...";
  bool probing = false;
  if (onSdAlready) {
    size[0] = '\0';
  } else if (book.length > 0) {
    formatFileSize(static_cast<uint64_t>(book.length), size, sizeof(size));
  } else {
    auto request = bookRequest(book);
    request.sizeOnly = true;
    probing = bookDownloader.start(std::move(request));
    if (!probing) snprintf(size, sizeof(size), "?");
  }
  if (size[0]) dialog->setNote(tr(STR_SIZE_LABEL), size, probing ? pollDownloadSize : nullptr, this);

  // entries and selectorIndex stay put while the dialog is on top (this
  // activity's loop does not run), so the index is enough to find the book.
  const int bookIndex = selectorIndex;
  startActivityForResult(std::move(dialog),
                         [this, bookIndex, path = std::move(path), serverFilename](const ActivityResult& result) {
                           bookDownloader.cancel();  // a size probe still running; downloadBook joins it
                           const OpdsEntry* book = rowEntry(bookIndex);
                           if (result.isCancelled || !book) return;
                           downloadBook(*book, path, nullptr, serverFilename);
                         });
}

bool OpdsBookBrowserActivity::pollDownloadSize(void* self, std::string& body) {
  const auto& downloader = static_cast<OpdsBookBrowserActivity*>(self)->bookDownloader;
  if (downloader.running()) return false;
  char size[16] = "?";
  if (downloader.total() > 0) formatFileSize(downloader.total(), size, sizeof(size));
  if (body == size) return false;
  body = size;
  return true;
}

OpdsBookDownloader::Request OpdsBookBrowserActivity::bookRequest(const OpdsEntry& book) const {
  // Relative to the current feed, not the root server URL.
  const std::string feedUrl = UrlUtils::buildUrl(server.url, currentPath);
  OpdsBookDownloader::Request request;
  request.url = UrlUtils::buildUrl(feedUrl, std::string(book.href));
  request.username = server.username;
  request.password = server.password;
  request.authorizationOrigin = UrlUtils::ensureProtocol(server.url);
  return request;
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book, const std::string& filename,
                                           const std::string* resumeValidator, const bool serverFilename) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = downloadTotal = 0;
  downloadReceiving = false;
  lastRenderedPercent = -1;
  lastProgressUpdateMs = millis();
  cancelDownload = false;
  requestUpdate(true);

#ifdef SIMULATOR
  downloadProgress = 1;
  downloadTotal = 2;
  downloadReceiving = true;
  requestUpdate(true);
  return;
#endif

  // A size probe from the download prompt may still be winding down.
  bookDownloader.cancel();
  bookDownloader.join();
  // The book download must not share the network or RAM with page preloads.
  stopPrefetch();
#if defined(FREEINK_NET_WOLFSSL)
  // The book goes over its own connection; don't hold a second TLS session's
  // RAM open for the whole download.
  if (feedConnection) feedConnection->end();
#endif

  // The folder from requestDownload, not the settings: they may have changed
  // since, and a retry must land next to its .part file.
  const std::string downloadFolder = FsHelpers::extractFolderPath(filename);
  if (downloadFolder != "/" && !Storage.exists(downloadFolder.c_str()) && !Storage.mkdir(downloadFolder.c_str())) {
    LOG_ERR("OPDS", "Could not create download folder %s", downloadFolder.c_str());
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }

  OpdsBookDownloader::Request request = bookRequest(book);
  request.path = filename;
  request.serverFilename = serverFilename;
  if (resumeValidator) {
    request.resume = true;
    request.validator = *resumeValidator;
  }
  if (!bookDownloader.start(std::move(request))) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
  }
  // loop() -> pollDownload() takes it from here.
}

void OpdsBookBrowserActivity::pollDownload() {
  // Touch goes through the app (the Cancel button sets cancelDownload). Back
  // cancels on press; its release must not also leave the listing.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) app.route(snap);
  }
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) cancelDownload = true;
  const bool backPressed = mappedInput.wasPressed(MappedInputManager::Button::Back);
  // cppcheck-suppress knownConditionTrueFalse ; app.route() can set cancelDownload via the Cancel button callback
  if ((cancelDownload || backPressed) && bookDownloader.running() && !bookDownloader.cancelling()) {
    LOG_INF("OPDS", "Download cancel requested");
    bookDownloader.cancel();
    if (backPressed) mappedInput.suppressNextBackRelease();
  }
  cancelDownload = false;

  if (bookDownloader.running()) {
    const bool receiving = bookDownloader.receiving();
    const size_t total = bookDownloader.total();
    const size_t downloaded = bookDownloader.downloaded();
    const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
    const unsigned long now = millis();
    // Redraw on the first byte, every few percent, and at least every few
    // seconds; each redraw is a full panel refresh.
    if (receiving != downloadReceiving ||
        (receiving && (percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
                       now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS))) {
      downloadReceiving = receiving;
      downloadProgress = downloaded;
      downloadTotal = total;
      lastRenderedPercent = percent;
      lastProgressUpdateMs = now;
      requestUpdate();
    }
    return;
  }

  const auto result = bookDownloader.result();
  const std::string& filename = bookDownloader.path();
  if (result == HttpDownloader::OK) {
    clearBookCache(filename);
    if (rowEntry(selectorIndex)) onSd.set(selectorIndex - windowBase);
    state = BrowserState::BROWSING;
    // The prompt draws over the last frame: show the list under it, not the
    // download screen (this also replaces the download frame's ghost).
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("OPDS", "List could not be rendered before the open prompt");
    }
    offerToOpen(filename);
  } else if (result == HttpDownloader::ABORTED) {
    LOG_INF("OPDS", "Download cancelled");
    state = BrowserState::BROWSING;
  } else if (result == HttpDownloader::INSUFFICIENT_SPACE) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_SD_CARD_FULL);
  } else if (result == HttpDownloader::FILE_EXISTS) {
    state = BrowserState::BROWSING;
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("OPDS", "List could not be rendered before the overwrite prompt");
    }
    offerOverwrite(filename);
    return;
  } else {
    state = BrowserState::BROWSING;
    if (requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      LOG_ERR("OPDS", "List could not be rendered before the retry prompt");
    }
    offerRetry(filename);
    return;
  }
  requestUpdate();
}

void OpdsBookBrowserActivity::offerRetry(const std::string& path) {
  LOG_INF("OPDS", "Download failed (error=%d), offering retry: %s", static_cast<int>(bookDownloader.result()),
          path.c_str());
  std::string heading = tr(STR_DOWNLOAD_FAILED);
  heading += ':';
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, heading, statusMessage);
  if (!dialog) {
    LOG_ERR("OPDS", "Cannot allocate retry dialog");
    Storage.remove((path + ".part").c_str());
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }
  dialog->setConfirmOption(tr(STR_RETRY), true);
  // As in requestDownload(): entries and selectorIndex stay put under the
  // dialog, so the index still names the failed book.
  const int bookIndex = selectorIndex;
  // Still pending: no response named the file, so nothing was written at path.
  const bool serverFilename = bookDownloader.serverFilenamePending();
  startActivityForResult(std::move(dialog), [this, bookIndex, path, serverFilename,
                                             validator = bookDownloader.validator()](const ActivityResult& result) {
    const OpdsEntry* book = rowEntry(bookIndex);
    if (result.isCancelled || !book) {
      LOG_INF("OPDS", "Download retry declined; removing partial file");
      if (!serverFilename) Storage.remove((path + ".part").c_str());
      return;
    }
    LOG_INF("OPDS", "Retrying download: %s", path.c_str());
    downloadBook(*book, path, &validator, serverFilename);
  });
}

void OpdsBookBrowserActivity::offerOverwrite(const std::string& path) {
  std::string details = path.substr(path.rfind('/') + 1);
  HalFile existing = Storage.open(path.c_str());
  if (existing && !existing.isDirectory()) {
    char sizeLabel[16];
    formatFileSize(existing.fileSize64(), sizeLabel, sizeof(sizeLabel));
    details += ", ";
    details += sizeLabel;
    char dateLabel[20];
    if (formatFatDateTime(existing.modificationTime(), dateLabel, sizeof(dateLabel))) {
      details += ", ";
      details += dateLabel;
    }
  }
  if (existing) existing.close();
  auto dialog = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_BOOK_EXISTS_OVERWRITE), details);
  if (!dialog) {
    LOG_ERR("OPDS", "Cannot allocate overwrite dialog");
    requestUpdate();
    return;
  }
  const int bookIndex = selectorIndex;
  startActivityForResult(std::move(dialog), [this, bookIndex, path](const ActivityResult& result) {
    const OpdsEntry* book = rowEntry(bookIndex);
    if (result.isCancelled || !book) return;
    // The name is settled: replace exactly that file, no second lookup.
    downloadBook(*book, path);
  });
}

void OpdsBookBrowserActivity::offerToOpen(const std::string& path) {
  auto dialog =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_OPEN_DOWNLOADED_BOOK), statusMessage);
  if (!dialog) {
    LOG_ERR("OPDS", "Cannot allocate open-book dialog");
    return;
  }
  startActivityForResult(std::move(dialog), [this, path](const ActivityResult& result) {
    if (result.isCancelled) return;
    LOG_INF("OPDS", "Opening downloaded book: %s", path.c_str());
    openAfterExit = path;
    onSelectBook(path);
  });
}

void OpdsBookBrowserActivity::launchSearch() {
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void OpdsBookBrowserActivity::performSearch(const std::string& query) {
  if (!query.empty() && searchTemplate.empty() && !searchDescriptionUrl.empty()) {
    // A few hundred bytes, fetched once: loadFeed() keeps it in the page cache.
    OpdsEntry unused[1];
    OpdsParser description(unused);
    if (loadFeed(UrlUtils::buildUrl(server.url, searchDescriptionUrl), description) && description) {
      searchTemplate = description.getSearchTemplate();
    }
    if (searchTemplate.empty())
      LOG_ERR("OPDS", "No search template in %s", UrlUtils::maskUserInfo(searchDescriptionUrl).c_str());
  }
  if (query.empty() || searchTemplate.empty()) {
    state = BrowserState::BROWSING;
    requestUpdate();
    return;
  }

  auto urlEncode = [](const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
      if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        out += static_cast<char>(c);
      else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", c);
        out += buf;
      }
    }
    return out;
  };

  std::string url = searchTemplate;
  const std::string placeholder = "{searchTerms}";
  const size_t pos = url.find(placeholder);
  if (pos != std::string::npos) url.replace(pos, placeholder.length(), urlEncode(query));

  pushHistory();
  currentPath = url;

  clearEntries();
  selectorIndex = 0;
  showLoadingBeforeFetch(url);
  fetchFeed(url);
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    showLoadingBeforeFetch(currentPath);
    fetchFeed(currentPath);
    return;
  }
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    showLoadingBeforeFetch(currentPath);
    fetchFeed(currentPath);
  } else {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}
