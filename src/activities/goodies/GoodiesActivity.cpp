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

#include <algorithm>
#include <atomic>
#include <cstring>

#include "CrossPointSettings.h"
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
#include "network/FirmwareFlasher.h"
#include "network/WifiUtils.h"
#include "util/TransferLightPulse.h"
#include "util/WorkerTask.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr char DISPLAY_TEST_DIR[] = "/debug/display";
constexpr size_t MAX_SD_TESTS = 64;

bool hasTxtExtension(const char* name, const size_t len) { return len > 4 && strcasecmp(name + len - 4, ".txt") == 0; }

std::unique_ptr<CrossPointWebServer> remoteServer;
std::string remoteIp;

// The toggle is SETTINGS.goodiesWifiRemote, so it survives Wi-Fi screens (File
// Transfer, Calibre, OPDS, Nearby), the silent restarts they end with, sleep and
// power-off. RTC_NOINIT keeps the network across ESP.restart() and deep sleep;
// after power-on it is garbage and the last connected network is used instead.
RTC_NOINIT_ATTR char remoteSsid[33];
bool rejoining = false;
bool rejoinNow = false;     // Toggled on in Goodies: skip the boot and idle waits
bool rejoinByUser = false;  // The attempt rejoinNow started; no network opens the picker
bool pickerRequested = false;
// The join itself (wifi.json read, WiFi.mode() bringing the driver up,
// WiFi.begin()) and the toggle-off teardown run on short-lived tasks so the
// main loop never waits on them. One at a time: radioTask runs both.
// Main task only, except radioTask's state and joinOutcome, which the tasks set.
bool joinPending = false;
WorkerTask radioTask;
std::atomic<uint8_t> joinOutcome{0};
enum JoinOutcome : uint8_t { JOIN_FAILED, JOIN_BEGUN, JOIN_NO_NETWORK };
constexpr uint32_t RADIO_TASK_STACK_BYTES = 6144;
enum class RadioOwner : uint8_t { None, Shared, Screen };
RadioOwner radioOwner = RadioOwner::None;
bool sharedStartTried = false;
uint32_t rejoinAt = 0;
uint32_t rejoinRetryMs = 0;  // 0 until an attempt fails; doubles per failure
constexpr uint32_t REJOIN_TIMEOUT_MS = 20000;
constexpr uint32_t REJOIN_RETRY_MIN_MS = 60000;
constexpr uint32_t REJOIN_RETRY_MAX_MS = 600000;
// The join task still shares the SD card and core 0 with the main loop, so a
// background join waits for the wake screen to paint and for a pause in input.
constexpr uint32_t REJOIN_BOOT_DELAY_MS = 5000;
constexpr uint32_t REJOIN_IDLE_MS = 2000;

bool remoteWanted() { return SETTINGS.goodiesWifiRemote != 0; }

// One settings.json write per toggle change; rejoins leave it alone.
void setRemoteWanted(const bool wanted) {
  if (remoteWanted() == wanted) return;
  SETTINGS.goodiesWifiRemote = wanted ? 1 : 0;
  if (!SETTINGS.saveToFile()) LOG_ERR("GDY", "wifi remote: toggle not saved");
}

void waitForRadioTask() {
  // Bounded by a wifi.json read and driver start, or a server stop and radio off; no Wi-Fi call may overlap it.
  while (radioTask.running()) vTaskDelay(1);
}

// Takes ownership of server (may be null). Runs on the main task or the shutdown task.
void closeServerAndRadio(CrossPointWebServer* server) {
  if (server) server->stop();
  delete server;
  MDNS.end();
  WiFi.disconnect(false);
  // As leaveNetworkInPlace(): the server's stop() left modem sleep off, which
  // Arduino would carry into the next Wi-Fi session.
  WiFi.setSleep(true);
  WiFi.mode(WIFI_OFF);
}

// Clears the main-task state and hands back the server to close.
CrossPointWebServer* detachServer() {
  waitForRadioTask();
  joinPending = false;
  remoteIp.clear();
  rejoining = false;
  return remoteServer.release();
}

void stopServerAndRadio() { closeServerAndRadio(detachServer()); }

void shutdownTaskMain(void* server) { closeServerAndRadio(static_cast<CrossPointWebServer*>(server)); }

// The toggle's Off: the server task handoff and radio off (~120 ms) run on a
// task. loop() and every Wi-Fi screen wait for it before touching the radio.
void stopServerAndRadioInBackground() {
  CrossPointWebServer* server = detachServer();
  if (!radioTask.start(shutdownTaskMain, server, RADIO_TASK_STACK_BYTES, "WifiOff")) {
    LOG_ERR("GDY", "wifi remote: shutdown task did not start, stopping inline");
    closeServerAndRadio(server);
  }
}

// Wi-Fi is already connected (WifiSelectionActivity succeeded, or a rejoin).
// On failure the radio is off (unless a screen owns it) and the toggle is left
// to the caller.
bool startRemote(const bool ownsRadio = true) {
  remoteServer = makeUniqueNoThrow<CrossPointWebServer>();
  if (remoteServer) remoteServer->begin(/*logOnly=*/true);
  if (!remoteServer || !remoteServer->isRunning()) {
    LOG_ERR("GDY", "wifi remote: web server did not start");
    if (ownsRadio) {
      stopServerAndRadio();
    } else {
      remoteServer.reset();
    }
    return false;
  }
  MDNS.begin("crosspoint");
  remoteIp = WiFi.localIP().toString().c_str();
  snprintf(remoteSsid, sizeof(remoteSsid), "%s", WiFi.SSID().c_str());
  rejoinRetryMs = 0;
  setRemoteWanted(true);
  LOG_INF("GDY", "wifi remote on: http://%s/api/psram-log", remoteIp.c_str());
  return true;
}

void joinTaskMain(void*) {
  const uint32_t startedAt = millis();
  remoteSsid[sizeof(remoteSsid) - 1] = '\0';
  auto cred = WIFI_STORE.findCredential(remoteSsid);
  if (!cred) cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  JoinOutcome outcome = JOIN_NO_NETWORK;
  if (cred) {
    WiFi.persistent(false);
    if (WiFi.mode(WIFI_STA)) {
      WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
      outcome = JOIN_BEGUN;
      LOG_INF("GDY", "wifi remote: rejoining %s (join task %lu ms)", cred->ssid.c_str(),
              static_cast<unsigned long>(millis() - startedAt));
    } else {
      outcome = JOIN_FAILED;
      LOG_ERR("GDY", "wifi remote: station mode failed");
    }
  } else {
    LOG_ERR("GDY", "wifi remote: no saved network to rejoin");
  }
  cred.reset();  // before the task frees its stack; the password copy lives on the heap
  joinOutcome.store(outcome, std::memory_order_relaxed);
}

// Background join of the network the remote last used. Every call counts as
// an attempt for the retry backoff.
void beginRejoin() {
  rejoinAt = millis();
  rejoinRetryMs = rejoinRetryMs == 0 ? REJOIN_RETRY_MIN_MS : std::min(rejoinRetryMs * 2, REJOIN_RETRY_MAX_MS);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (largest < MemoryBudget::OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC) {
    LOG_ERR("GDY", "wifi remote: rejoin skipped, internal largest block %u", static_cast<unsigned>(largest));
    return;
  }
  // Priority 1, under the main loop: it runs only while the loop waits. The
  // stack is internal RAM (it reads the SD card) and is freed when the task ends.
  if (!radioTask.start(joinTaskMain, nullptr, RADIO_TASK_STACK_BYTES, "WifiJoin")) {
    LOG_ERR("GDY", "wifi remote: join task did not start");
    return;
  }
  joinPending = true;
}
}  // namespace

namespace goodies_remote {
bool running() { return remoteServer && remoteServer->isRunning() && WiFi.status() == WL_CONNECTED; }

bool wanted() { return remoteWanted(); }

bool allowsRadioIdleSleep() {
  return remoteServer && remoteServer->allowsIdleSleep() && !remoteServer->isTransferActive() &&
         !activityManager.anyActivityUsesWifi();
}

void waitForJoin() { waitForRadioTask(); }

bool takePickerRequest() {
  const bool requested = pickerRequested;
  pickerRequested = false;
  return requested;
}

void pause() {
  if (!remoteServer && !rejoining && !joinPending) return;
  stopServerAndRadio();
  LOG_INF("GDY", "wifi remote paused");
}

void stop() {
  setRemoteWanted(false);
  rejoinNow = false;
  if (!remoteServer && !rejoining && !joinPending) return;
  stopServerAndRadioInBackground();
  LOG_INF("GDY", "wifi remote off");
}

void startInBackground() {
  stop();  // a server left behind when the link dropped; no-op otherwise
  setRemoteWanted(true);
  rejoinAt = 0;
  rejoinRetryMs = 0;
  rejoinNow = true;
}

// /api/ota on the remote's server pulses the frontlight like a file transfer
// (File Transfer's own server has its pulse). Ends once the stream has been
// closed for a full pulse cycle.
void updateOtaLight() {
  static TransferLightPulse light;
  static bool lit = false;
  static uint32_t lastStreamMs = 0;
  const bool streaming = firmware_flash::streamActive();
  if (streaming) lastStreamMs = millis();
  if (streaming && !lit && remoteServer) {
    light.begin(/*holdMs=*/0);
    lit = true;
  }
  if (!lit) return;
  light.update(streaming);
  if (!streaming && millis() - lastStreamMs > 1500) {
    light.end();
    lit = false;
  }
}

void loop(const uint32_t idleMs) {
  updateOtaLight();
  if (!remoteWanted()) return;
  // A join or a toggle-off teardown still owns the radio (Off then On in quick succession).
  if (radioTask.running()) return;
  if (joinPending) {
    joinPending = false;
    const uint8_t outcome = joinOutcome.load(std::memory_order_relaxed);
    if (outcome == JOIN_BEGUN) {
      rejoining = true;
      rejoinAt = millis();  // the connect timeout runs from WiFi.begin()
    } else if (outcome == JOIN_NO_NETWORK && rejoinByUser) {
      // Toggled on with no saved network: Goodies opens the Wi-Fi picker.
      setRemoteWanted(false);
      pickerRequested = true;
    }
    rejoinByUser = false;
    if (!rejoining) return;
  }
  // A Wi-Fi screen on the stack owns the radio until it leaves. Shared: all of
  // them (OPDS) only make HTTP requests, so the remote serves on their link.
  const RadioOwner owner = !activityManager.anyActivityUsesWifi()        ? RadioOwner::None
                           : activityManager.wifiActivitiesShareRemote() ? RadioOwner::Shared
                                                                         : RadioOwner::Screen;
  if (owner != radioOwner) {
    const RadioOwner left = radioOwner;
    radioOwner = owner;
    sharedStartTried = false;
    if (left != RadioOwner::None && owner != RadioOwner::Screen) {
      // Whatever the screen left behind (Wi-Fi off or deinitialized, another
      // network, AP mode), the old server's sockets can't be trusted.
      if (remoteServer) remoteServer->stop();
      remoteServer.reset();
      MDNS.end();
      rejoinAt = 0;
      rejoinRetryMs = 0;
      if (owner == RadioOwner::None) {
        if (hasActiveStationWifiConnection()) {
          if (!startRemote()) rejoinAt = millis();
          return;
        }
        if (WiFi.getMode() != WIFI_MODE_NULL) stopServerAndRadio();
      }
    }
  }
  if (owner == RadioOwner::Screen) {
    rejoining = false;
    return;
  }
  if (owner == RadioOwner::Shared) {
    // Never joins or powers the radio here: the screen does. One start per
    // shared stretch, once there is a link and the heap Wi-Fi entry wants.
    rejoining = false;
    if (!remoteServer && !sharedStartTried && hasActiveStationWifiConnection() &&
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= MemoryBudget::OPTIONAL_EPUB_REBUILD_MIN_FREE &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= MemoryBudget::OPTIONAL_EPUB_REBUILD_MIN_MAX_ALLOC) {
      sharedStartTried = true;
      startRemote(/*ownsRadio=*/false);
    }
    return;
  }
  if (rejoining) {
    if (WiFi.status() == WL_CONNECTED) {
      rejoining = false;
      if (!startRemote()) rejoinAt = millis();
    } else if (millis() - rejoinAt > REJOIN_TIMEOUT_MS) {
      LOG_ERR("GDY", "wifi remote: rejoin timed out, retrying in %lu s",
              static_cast<unsigned long>(rejoinRetryMs / 1000));
      stopServerAndRadio();
      rejoinAt = millis();
    }
    return;
  }
  if (remoteServer) return;
  if (!rejoinNow) {
    if (millis() < REJOIN_BOOT_DELAY_MS || idleMs < REJOIN_IDLE_MS) return;
    if (rejoinAt != 0 && millis() - rejoinAt < rejoinRetryMs) return;
  }
  rejoinByUser = rejoinNow;
  rejoinNow = false;
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
  goodies_remote::takePickerRequest();  // left over from a toggle made on an earlier visit
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
    entries.push_back({tr(STR_WIFI_REMOTE), -1, {}, remoteRowValue()});
    remoteRowShown = remoteRowState();
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
  selectedIndex = index;  // a tapped row becomes the selected one, as on every other list
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

int GoodiesActivity::remoteRowState() {
  if (goodies_remote::running()) return 2;
  return goodies_remote::wanted() ? 1 : 0;
}

std::string GoodiesActivity::remoteRowValue() {
  switch (remoteRowState()) {
    case 2:
      return remoteIp;
    case 1:
      return tr(STR_CONNECTING);
    default:
      return tr(STR_STATE_OFF);
  }
}

void GoodiesActivity::toggleRemote() {
  if (goodies_remote::wanted()) {
    goodies_remote::stop();
  } else {
    // Joins the saved network in the background (the row reads Connecting...).
    goodies_remote::startInBackground();
  }
  refreshRemoteRow();
}

// Updates the remote row in place; showLevel() would move the selection back to the top.
void GoodiesActivity::refreshRemoteRow() {
  RenderLock lock(*this);
  remoteRowShown = remoteRowState();
  entries[1].value = remoteRowValue();
  rowItems[1].value = entries[1].value.c_str();
  lock.unlock();
  requestUpdate();
}

void GoodiesActivity::openRemotePicker() {
  // No saved network: the Wi-Fi picker joins one and leaves Wi-Fi up on success.
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           mappedInput.suppressNextConfirmRelease();
                           if (!result.isCancelled) startRemote();
                           if (level == Level::Root) {
                             refreshRemoteRow();
                           } else {
                             showLevel(Level::Root);
                           }
                         });
}

void GoodiesActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  // Acted on after route() returns: showLevel() rebuilds the rows it iterates.
  static_cast<GoodiesActivity*>(user)->pendingRow = event.value;
}

void GoodiesActivity::loop() {
  if (goodies_remote::takePickerRequest()) {
    openRemotePicker();
    return;
  }
  // The background join finishes (or drops) while this screen is open.
  if (level == Level::Root && entries.size() > 1 && remoteRowState() != remoteRowShown) refreshRemoteRow();
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
