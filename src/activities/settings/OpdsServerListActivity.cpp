#include "OpdsServerListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "OpdsSettingsActivity.h"
#include "activities/ActivityManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "components/icons/markIcons.h"
#include "fontIds.h"
#include "network/WifiBackgroundJoin.h"
#include "network/WifiUtils.h"
#include "util/UrlUtils.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

std::string normalizeDownloadFolder(std::string folder) {
  while (!folder.empty() && (folder.front() == ' ' || folder.front() == '\t')) folder.erase(folder.begin());
  while (!folder.empty() && (folder.back() == ' ' || folder.back() == '\t')) folder.pop_back();
  if (folder.empty() || folder == "/") return "";
  if (folder.front() != '/') folder.insert(folder.begin(), '/');
  while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
  return folder;
}
}  // namespace

int OpdsServerListActivity::getItemCount() const {
  int count = static_cast<int>(OPDS_STORE.getCount());
  // Both modes append "Add Server"; Settings also includes Download Folder.
  count += pickerMode ? 1 : 2;
  return count;
}

OpdsServerListActivity::OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                               const bool pickerMode)
    : Activity("OpdsServerList", renderer, mappedInput),
      pickerMode(pickerMode),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void OpdsServerListActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<OpdsServerListActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(self->getItemCount())) return;
  self->selectedIndex = event.value;
  // Activation opens an editor/browser or repaints a new value; a lingering
  // flash would gray an unrelated row.
  self->app.clearTapFlash();
  self->handleSelection();
  self->requestUpdate();
}

void OpdsServerListActivity::onEnter() {
  Activity::onEnter();

  // Reload from disk in case servers were added/removed by a subactivity or the web UI
  OPDS_STORE.loadFromFile();
  selectedIndex = 0;
  uiReady = false;
  visibleRows = 1;
  topIndex = 0;
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &OpdsServerListActivity::onRowEvent, this);
  app.setScreen(&OpdsServerListActivity::listScreen, this);
  requestUpdate();

  // Silent, no screen: the join runs on its own task while the list draws and
  // takes input, and the browser skips the Wi-Fi screen if it got the link.
  leavingToBrowser = false;
  if (pickerMode) {
    wifi_background_join::start();
    rootsQueued = false;
    // Back from a catalog: its page cache comes back with the roots already in it.
    if (!pageCache) pageCache = opds_page_cache_handoff::take();
    if (pageCache) {
      // Marks for roots already cached show on the first frame, not after the pool.
      pageCachedAt = pageCache->changes();
      rootCached.reset();
      const auto& servers = OPDS_STORE.getServers();
      for (size_t i = 0; i < servers.size() && i < rootCached.size(); ++i) {
        if (pageCache->contains(UrlUtils::buildUrl(servers[i].url, ""))) rootCached.set(i);
      }
    }
    if (!pageCache && psramHeapAvailable()) {
      const size_t budget = std::min(OPDS_PAGE_CACHE_MAX_BYTES, byteHeapSnapshot(MemoryPool::Psram).free / 4);
      pageCache = makeUniqueNoThrow<OpdsPageCache>(budget);
      rootCached.reset();
      pageCachedAt = 0;
    }
    if (pageCache && !preload) {
      // Credentials come with each page: the servers differ.
      preload = makeUniqueNoThrow<OpdsPreloadPool>(*pageCache, OPDS_PAGE_MAX_BYTES, "", "", "");
      if (!preload) LOG_ERR("OPDS", "OOM: server list preload pool");
    }
  }
}

// Queues every server's root page once Wi-Fi is up and pumps the pool (up to
// three fetches at once, as the browser preloads a catalog's pages). Pages that
// land mark their rows; a server that fails just stays unmarked.
void OpdsServerListActivity::pumpPrefetch() {
#ifndef SIMULATOR
  if (!preload || !hasActiveStationWifiConnection()) return;
  const auto& servers = OPDS_STORE.getServers();
  if (!rootsQueued) {
    rootsQueued = true;
    for (const auto& server : servers) {
      if (server.url.empty()) continue;
      preload->enqueue(UrlUtils::buildUrl(server.url, ""), false, server.username, server.password,
                       UrlUtils::ensureProtocol(server.url));
    }
  }
  preload->pump();
  if (pageCache->changes() != pageCachedAt) {
    pageCachedAt = pageCache->changes();
    std::bitset<OpdsServerStore::MAX_SERVERS> cached;
    for (size_t i = 0; i < servers.size() && i < cached.size(); ++i) {
      if (pageCache->contains(UrlUtils::buildUrl(servers[i].url, ""))) cached.set(i);
    }
    if (cached != rootCached) {
      RenderLock lock(*this);
      rootCached = cached;
      if (marksPendingMs == 0) marksPendingMs = millis() | 1;
    }
  }
  // One redraw per batch (a repaint the user causes meanwhile shows the marks
  // landed so far): each page redrawing on its own cost two or three e-ink
  // refreshes and queued taps behind them.
  if (preload->marksDue(marksPendingMs)) requestUpdate();
#endif
}

void OpdsServerListActivity::onExit() {
  Activity::onExit();
  preload.reset();  // joins a fetch in flight before the cache goes
  if (pickerMode && leavingToBrowser) {
    opds_page_cache_handoff::give(std::move(pageCache));
  } else {
    pageCache.reset();
  }
  // Back to Home or into the server editor: the link is not needed any more.
  if (pickerMode && !leavingToBrowser) wifi_background_join::stop();
}

// Not while the join or a root-page fetch is running, as the browser's list.
bool OpdsServerListActivity::allowsRadioIdleSleep() {
  return !wifi_background_join::joining() && !(preload && preload->busy());
}

void OpdsServerListActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;
  if (pickerMode) pumpPrefetch();

  auto activateSelected = [this] { handleSelection(); };

  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (pickerMode) {
      activityManager.goHome(HomeMenuItem::OPDS_BROWSER);
    } else {
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateSelected();
    return;
  }

  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and let onRowEvent dispatch.
  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;  // dispatched to onRowEvent
    }
  }

  const int itemCount = getItemCount();
  if (itemCount > 0) {
    // Swipes scroll the viewport; the selection stays put and button
    // navigation pulls the view back to it.
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
      const int next = scrollListBy(topIndex, delta, visibleRows, itemCount);
      if (next != topIndex) {
        topIndex = next;
        requestUpdate();
      }
      return;
    }
    const auto moveSelection = [this, itemCount](const int index) {
      selectedIndex = index;
      topIndex = followListSelection(selectedIndex, topIndex, visibleRows, itemCount);
      requestUpdate();
    };
    buttonNavigator.onNext(
        [this, itemCount, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedIndex, itemCount)); });
    buttonNavigator.onPrevious(
        [this, itemCount, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectedIndex, itemCount)); });
  }
}

void OpdsServerListActivity::handleSelection() {
  const auto serverCount = static_cast<int>(OPDS_STORE.getCount());

  if (pickerMode) {
    // Picker mode: select a server or add the first one without leaving the flow.
    if (selectedIndex < serverCount) {
      leavingToBrowser = true;
      activityManager.goToOpdsServer(static_cast<uint32_t>(selectedIndex), false, true);
    } else {
      auto editor = makeUniqueNoThrow<OpdsSettingsActivity>(renderer, mappedInput, -1);
      if (!editor) {
        LOG_ERR("OPDS", "OOM: OPDS settings activity");
        return;
      }
      startActivityForResult(std::move(editor), [this](const ActivityResult&) {
        OPDS_STORE.loadFromFile();
        selectedIndex = 0;
        topIndex = 0;
        rootsQueued = false;  // a new server gets its page too
        requestUpdate();
      });
    }
    return;
  }

  // Item layout: configured servers, Add Server, Download Folder.
  if (selectedIndex == serverCount + 1) {
    auto resultHandler = [this](const ActivityResult& result) {
      if (result.isCancelled) return;

      const auto& keyboardResult = std::get<KeyboardResult>(result.data);
      const std::string folder = normalizeDownloadFolder(keyboardResult.text);
      strncpy(SETTINGS.opdsDownloadFolder, folder.c_str(), sizeof(SETTINGS.opdsDownloadFolder) - 1);
      SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
      if (!SETTINGS.saveToFile()) {
        LOG_ERR("OPDS", "Could not save download folder setting");
      }
      requestUpdate();
    };
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                               renderer, mappedInput, tr(STR_OPDS_DOWNLOAD_FOLDER), SETTINGS.opdsDownloadFolder,
                               sizeof(SETTINGS.opdsDownloadFolder) - 1, InputType::Text),
                           resultHandler);
    return;
  }

  // Settings mode: open editor for selected server, or create a new one
  auto resultHandler = [this](const ActivityResult&) {
    // Reload server list when returning from editor
    OPDS_STORE.loadFromFile();
    selectedIndex = 0;
  };

  if (selectedIndex < serverCount) {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, selectedIndex), resultHandler);
  } else {
    startActivityForResult(std::make_unique<OpdsSettingsActivity>(renderer, mappedInput, -1), resultHandler);
  }
}

void OpdsServerListActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<OpdsServerListActivity*>(user)->buildListScreen(screen);
}

void OpdsServerListActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Content below the GUI.drawHeader band, above the button hints.
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int itemCount = getItemCount();
  if (itemCount == 0) {
    screen.centeredText(tr(STR_NO_SERVERS), screen.theme().bodyText);
    return;
  }

  const auto& servers = OPDS_STORE.getServers();
  const auto serverCount = static_cast<int>(servers.size());

  // Primary label: server name (falling back to URL if unnamed); subtitle is
  // the URL when a name is set, or the current folder/format values.
  std::vector<fui::ListItem> items;
  items.reserve(itemCount);
  // As the browser's rows: a check in the row inset for a page already cached.
  const fui::BitmapRef mark = fui::bitmapFromIcon(icon_check_20);
  for (int i = 0; i < serverCount; i++) {
    fui::ListItem item;
    item.label = servers[i].name.empty() ? servers[i].url.c_str() : servers[i].name.c_str();
    if (!servers[i].name.empty()) item.subtitle = servers[i].url.c_str();
    if (static_cast<size_t>(i) < rootCached.size() && rootCached[i]) item.icon = mark;
    item.actionValue = static_cast<int16_t>(i);
    items.push_back(item);
  }
  fui::ListItem addServer;
  addServer.label = tr(STR_ADD_SERVER);
  addServer.actionValue = static_cast<int16_t>(serverCount);
  items.push_back(addServer);

  if (!pickerMode) {
    fui::ListItem folder;
    folder.label = tr(STR_OPDS_DOWNLOAD_FOLDER);
    folder.subtitle = SETTINGS.opdsDownloadFolder[0] ? SETTINGS.opdsDownloadFolder : tr(STR_OPDS_SD_ROOT);
    folder.actionValue = static_cast<int16_t>(serverCount + 1);
    items.push_back(folder);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in loop()
  props.iconsInMargin = true;         // the check sits left of the title, as in the catalog: nothing moves
  const auto rows = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, itemCount);  // clamp to range
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void OpdsServerListActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_OPDS_SERVERS), false);

  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
