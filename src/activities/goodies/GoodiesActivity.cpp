#include "GoodiesActivity.h"

#if CROSSDINK_GOODIES

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <algorithm>
#include <cstring>

#include "DisplayScript.h"
#include "DisplayTestActivity.h"
#include "MappedInputManager.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/CrossPointWebServer.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr char DISPLAY_TEST_DIR[] = "/debug/display";
constexpr size_t MAX_SD_TESTS = 64;

bool hasTxtExtension(const char* name, const size_t len) { return len > 4 && strcasecmp(name + len - 4, ".txt") == 0; }

std::unique_ptr<CrossPointWebServer> remoteServer;
std::string remoteIp;

// The toggle survives Wi-Fi screens (File Transfer, Calibre, OPDS, Nearby) and
// the silent restarts they end with: RTC_NOINIT keeps it across ESP.restart(),
// and any other boot (power-on, deep-sleep wake) clears it.
constexpr uint32_t REMOTE_WANTED_MAGIC = 0x57524d54;  // "WRMT"
RTC_NOINIT_ATTR uint32_t remoteWantedMagic;
RTC_NOINIT_ATTR char remoteSsid[33];
bool rejoining = false;
uint32_t rejoinAt = 0;
constexpr uint32_t REJOIN_TIMEOUT_MS = 20000;
constexpr uint32_t REJOIN_RETRY_MS = 60000;

bool remoteWanted() { return remoteWantedMagic == REMOTE_WANTED_MAGIC; }

void stopServerAndRadio() {
  if (remoteServer) remoteServer->stop();
  remoteServer.reset();
  MDNS.end();
  WiFi.disconnect(false);
  // As leaveNetworkInPlace(): the server's stop() left modem sleep off, which
  // Arduino would carry into the next Wi-Fi session.
  WiFi.setSleep(true);
  WiFi.mode(WIFI_OFF);
  remoteIp.clear();
  rejoining = false;
}

// Wi-Fi is already connected (WifiSelectionActivity succeeded, or a rejoin).
bool startRemote() {
  remoteServer = makeUniqueNoThrow<CrossPointWebServer>();
  if (remoteServer) remoteServer->begin(/*logOnly=*/true);
  if (!remoteServer || !remoteServer->isRunning()) {
    LOG_ERR("GDY", "wifi remote: web server did not start");
    goodies_remote::stop();
    return false;
  }
  MDNS.begin("crosspoint");
  remoteIp = WiFi.localIP().toString().c_str();
  snprintf(remoteSsid, sizeof(remoteSsid), "%s", WiFi.SSID().c_str());
  remoteWantedMagic = REMOTE_WANTED_MAGIC;
  LOG_INF("GDY", "wifi remote on: http://%s/api/psram-log", remoteIp.c_str());
  return true;
}

// Background join of the network the remote last used (async WiFi.begin()).
void beginRejoin() {
  rejoinAt = millis();
  const auto cred = WIFI_STORE.findCredential(remoteSsid);
  if (!cred) {
    LOG_ERR("GDY", "wifi remote: no saved credential for %s", remoteSsid);
    return;
  }
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (largest < MemoryBudget::OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC) {
    LOG_ERR("GDY", "wifi remote: rejoin skipped, internal largest block %u", static_cast<unsigned>(largest));
    return;
  }
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("GDY", "wifi remote: station mode failed");
    return;
  }
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
  rejoining = true;
  LOG_INF("GDY", "wifi remote: rejoining %s", remoteSsid);
}
}  // namespace

namespace goodies_remote {
bool running() { return remoteServer && remoteServer->isRunning() && WiFi.status() == WL_CONNECTED; }

void pause() {
  if (!remoteServer && !rejoining) return;
  stopServerAndRadio();
  LOG_INF("GDY", "wifi remote paused");
}

void stop() {
  remoteWantedMagic = 0;
  if (!remoteServer && !rejoining) return;
  stopServerAndRadio();
  LOG_INF("GDY", "wifi remote off");
}

void loop() {
  static bool bootChecked = false;
  if (!bootChecked) {
    bootChecked = true;
    if (esp_reset_reason() != ESP_RST_SW) remoteWantedMagic = 0;
  }
  if (!remoteWanted()) return;
  if (rejoining) {
    if (WiFi.status() == WL_CONNECTED) {
      rejoining = false;
      rejoinAt = 0;
      startRemote();
    } else if (millis() - rejoinAt > REJOIN_TIMEOUT_MS) {
      LOG_ERR("GDY", "wifi remote: rejoin timed out, retrying in %u s", static_cast<unsigned>(REJOIN_RETRY_MS / 1000));
      stopServerAndRadio();
      rejoinAt = millis();
    }
    return;
  }
  // Radio on: the remote or a Wi-Fi screen owns it. Off at Home or in the
  // reader: a Wi-Fi screen (File Transfer, OPDS, Nearby...) turned it off on
  // exit, or this boot followed a silent restart.
  if (WiFi.getMode() != WIFI_MODE_NULL) return;
  if (!activityManager.isHomeActivity() && !activityManager.isReaderActivity()) return;
  if (remoteServer) {
    // Served through a screen that has since deinitialized Wi-Fi (OPDS).
    remoteServer->stop();
    remoteServer.reset();
    MDNS.end();
  }
  if (rejoinAt != 0 && millis() - rejoinAt < REJOIN_RETRY_MS) return;
  beginRejoin();
}
}  // namespace goodies_remote

GoodiesActivity::GoodiesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Goodies", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void GoodiesActivity::onEnter() {
  Activity::onEnter();
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &GoodiesActivity::onRowEvent, this);
  app.setScreen(&GoodiesActivity::listScreen, this);
  showLevel(Level::Root);
}

void GoodiesActivity::showLevel(const Level next) {
  RenderLock lock(*this);
  level = next;
  entries.clear();
  if (level == Level::Root) {
    entries.push_back({tr(STR_DISPLAY_TEST), -1, {}});
    entries.push_back({tr(STR_WIFI_REMOTE), -1, {}, goodies_remote::running() ? remoteIp : tr(STR_STATE_OFF)});
  } else {
    entries.reserve(display_script::BUILT_IN_COUNT + 8);
    for (int i = 0; i < display_script::BUILT_IN_COUNT; ++i) {
      entries.push_back({display_script::BUILT_INS[i].name, i, {}});
    }
    // AI- or hand-written tests: /debug/display/*.txt, sorted by name.
    const size_t firstSd = entries.size();
    FsFile dir = Storage.open(DISPLAY_TEST_DIR);
    if (dir && dir.isDirectory()) {
      char name[96];
      for (FsFile file = dir.openNextFile(); file && entries.size() - firstSd < MAX_SD_TESTS;
           file = dir.openNextFile()) {
        const size_t len = file.getName(name, sizeof(name));
        const bool script = !file.isDirectory() && name[0] != '.' && hasTxtExtension(name, len);
        file.close();
        if (!script) continue;
        entries.push_back({std::string(name, len - 4), -1, std::string(DISPLAY_TEST_DIR) + "/" + name});
      }
    }
    if (dir) dir.close();
    std::sort(entries.begin() + firstSd, entries.end(),
              [](const Entry& a, const Entry& b) { return a.label < b.label; });
  }
  rowItems.assign(entries.size(), fui::ListItem{});
  for (size_t i = 0; i < entries.size(); ++i) {
    rowItems[i].label = entries[i].label.c_str();
    rowItems[i].actionValue = static_cast<int16_t>(i);
    if (!entries[i].value.empty()) rowItems[i].value = entries[i].value.c_str();
  }
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  lock.unlock();
  requestUpdate();
}

void GoodiesActivity::activate(const int index) {
  if (index < 0 || index >= static_cast<int>(entries.size())) return;
  app.clearTapFlash();
  if (level == Level::Root) {
    if (index == 0) {
      showLevel(Level::DisplayTests);
    } else {
      toggleRemote();
    }
    return;
  }
  const Entry& entry = entries[index];
  startActivityForResult(
      std::make_unique<DisplayTestActivity>(renderer, mappedInput, entry.label, entry.builtIn, entry.path),
      [this](const ActivityResult&) {
        mappedInput.suppressNextConfirmRelease();
        requestUpdate();
      });
}

void GoodiesActivity::toggleRemote() {
  if (goodies_remote::running()) {
    goodies_remote::stop();
    showLevel(Level::Root);
    return;
  }
  goodies_remote::stop();  // a server left behind when the link dropped; no-op otherwise
  // The Wi-Fi picker auto-joins a saved network and leaves Wi-Fi up on success.
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           mappedInput.suppressNextConfirmRelease();
                           if (!result.isCancelled) startRemote();
                           showLevel(Level::Root);
                         });
}

void GoodiesActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  // Acted on after route() returns: showLevel() rebuilds the rows it iterates.
  static_cast<GoodiesActivity*>(user)->pendingRow = event.value;
}

void GoodiesActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    if (level == Level::Root) {
      finish();
    } else {
      showLevel(Level::Root);
    }
    return;
  }
  if (uiReady) {
    const auto snapshot = touchSnapshotFrom(mappedInput);
    if (snapshot.touchPressed || snapshot.touchReleased) {
      pendingRow = -1;
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (pendingRow >= 0) activate(pendingRow);
      if (event) return;
    }
  }
  const int count = static_cast<int>(entries.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selectedIndex);
    return;
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, count);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto move = [this, count](const int index) {
    selectedIndex = index;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, count);
    requestUpdate();
  };
  buttonNavigator.onNextRelease([&move, this, count] { move(ButtonNavigator::nextIndex(selectedIndex, count)); });
  buttonNavigator.onPreviousRelease(
      [&move, this, count] { move(ButtonNavigator::previousIndex(selectedIndex, count)); });
  buttonNavigator.onNextContinuous(
      [&move, this, count] { move(ButtonNavigator::nextPageIndex(selectedIndex, count, visibleRows)); });
  buttonNavigator.onPreviousContinuous(
      [&move, this, count] { move(ButtonNavigator::previousPageIndex(selectedIndex, count, visibleRows)); });
}

void GoodiesActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<GoodiesActivity*>(user)->buildListScreen(screen);
}

void GoodiesActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMargin(
      fui::Insets{static_cast<int16_t>(metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)), 0,
                  static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int count = static_cast<int>(rowItems.size());
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(count);
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, count);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void GoodiesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const char* title = level == Level::Root ? tr(STR_GOODIES) : tr(STR_DISPLAY_TEST);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false);
  } else {
    GUI.drawHeader(renderer, header, title);
  }
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSDINK_GOODIES
