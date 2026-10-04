#include "GoodiesActivity.h"

#if CROSSDINK_GOODIES

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Knobs.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cstring>

#include "BatteryStatsActivity.h"
#include "CrossPointSettings.h"
#include "DisplayScript.h"
#include "DisplayTestActivity.h"
#include "GlobalActions.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/IntervalSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "network/CrossPointWebServer.h"
#include "network/FirmwareFlasher.h"
#include "network/NetworkName.h"
#include "network/SerialRemote.h"
#include "network/WifiUtils.h"
#include "platform/PinMon.h"
#include "util/SleepLog.h"
#include "util/TransferLightPulse.h"
#include "util/WorkerTask.h"

namespace fui = freeink::ui;
namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr fui::ActionId ACTION_TAB = 2;
constexpr fui::ActionId ACTION_STEP = 3;  // Keyboard test - / +: value = row * 2 (+ 1 for +)
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
// A screen took the radio (pause): rejoin as soon as it leaves, no idle wait.
bool rejoinAfterPause = false;
bool pickerRequested = false;
// The join itself (wifi.json read, WiFi.mode() bringing the driver up,
// WiFi.begin()) and the toggle-off teardown run on short-lived tasks so the
// main loop never waits on them. One at a time: radioTask runs both.
// Main task only, except radioTask's state and joinOutcome, which the tasks set.
bool joinPending = false;
bool shutdownQueued = false;  // Off during a join: loop() shuts down once it ends
WorkerTask radioTask;
std::atomic<uint8_t> joinOutcome{0};
enum JoinOutcome : uint8_t { JOIN_FAILED, JOIN_BEGUN, JOIN_NO_NETWORK };
constexpr uint32_t RADIO_TASK_STACK_BYTES = 6144;
enum class RadioOwner : uint8_t { None, Shared, Screen };
RadioOwner radioOwner = RadioOwner::None;
bool sharedStartTried = false;
uint32_t rejoinAt = 0;
uint32_t rejoinRetryMs = 0;                      // 0 until an attempt fails; doubles per failure
KNOB_ALIAS(REJOIN_TIMEOUT_MS, rejoinTimeoutMs);  // Goodies > Knobs, as the rejoin times below
KNOB_ALIAS(REJOIN_RETRY_MIN_MS, rejoinRetryMinMs);
KNOB_ALIAS(REJOIN_RETRY_MAX_MS, rejoinRetryMaxMs);
// Internal RAM a remote join or server start needs. Allocations over 1 KB and the Wi-Fi/lwIP
// buffers go to PSRAM (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP),
// so the largest internal need is a task stack (join task 6 KB, Wi-Fi and lwIP tasks).
KNOB_ALIAS(WIFI_REMOTE_MIN_INTERNAL_FREE, wifiRemoteMinFree);  // Goodies > Knobs
KNOB_ALIAS(WIFI_REMOTE_MIN_INTERNAL_BLOCK, wifiRemoteMinBlock);
bool rejoinSkippedForMemory = false;  // the last rejoin was skipped for internal RAM

bool hasInternalHeapForRemote() {
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= WIFI_REMOTE_MIN_INTERNAL_FREE &&
         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= WIFI_REMOTE_MIN_INTERNAL_BLOCK;
}
// The join task still shares the SD card and core 0 with the main loop, so a
// background join waits for the wake screen to paint and for a pause in input.
KNOB_ALIAS(REJOIN_BOOT_DELAY_MS, rejoinBootDelayMs);
KNOB_ALIAS(REJOIN_IDLE_MS, rejoinIdleMs);

bool remoteWanted() { return SETTINGS.goodiesWifiRemote != 0; }

// One settings.json write per toggle change; rejoins leave it alone.
void setRemoteWanted(const bool wanted) {
  if (remoteWanted() == wanted) return;
  SETTINGS.goodiesWifiRemote = wanted ? 1 : 0;
  if (!SETTINGS.saveToFile()) LOG_ERR("GDY", "wifi remote: toggle not saved");
}

// Callers that must own the radio next (a Wi-Fi screen, deep sleep, an inline
// stop) wait here. Each of them takes the radio over or turns it off itself,
// so an Off queued behind the join is simply dropped.
void waitForRadioTask() {
  // Bounded by a wifi.json read and driver start, or a server stop and radio off; no Wi-Fi call may overlap it.
  while (radioTask.running()) vTaskDelay(1);
  shutdownQueued = false;
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
  logInternalHeapPins("wifi remote off");
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
  if (radioTask.running()) {
    // A join is in flight (no server yet): never overlap its Wi-Fi calls and
    // never wait for it here. loop() runs this again once it has ended.
    shutdownQueued = true;
    joinPending = false;
    rejoining = false;
    return;
  }
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
  MDNS.begin(NET_HOSTNAME);
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
      WiFi.setSleep(false);  // as WifiSelection: awake for DHCP; the server's begin() turns it back on
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
  rejoinSkippedForMemory = !hasInternalHeapForRemote();
  if (rejoinSkippedForMemory) {
    // Goodies shows it on the remote row; the retry backoff tries again.
    LOG_ERR("GDY", "wifi remote: rejoin skipped, internal free %u largest block %u",
            static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    logInternalHeapPins("rejoin skipped");
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

bool waitingForMemory() { return rejoinSkippedForMemory && remoteWanted() && !remoteServer; }

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

bool keepsStation() {
  if (!remoteWanted() || WiFi.getMode() != WIFI_MODE_STA || !hasActiveStationWifiConnection()) return false;
  remoteSsid[sizeof(remoteSsid) - 1] = '\0';
  return WiFi.SSID() == remoteSsid;
}

void pause(const bool keepStation) {
  rejoinAfterPause = true;
  if (!remoteServer && !rejoining && !joinPending) return;
  if (keepStation && keepsStation()) {
    // The screen reuses the association and adopts the server (takeServer).
    LOG_INF("GDY", "wifi remote paused, link and server kept for the screen");
    return;
  }
  stopServerAndRadio();
  LOG_INF("GDY", "wifi remote paused");
}

std::unique_ptr<CrossPointWebServer> takeServer() {
  if (!remoteServer) return {};
  std::unique_ptr<CrossPointWebServer> server(detachServer());
  if (keepsStation() && server->upgradeToFull()) {
    LOG_INF("GDY", "wifi remote: server handed to the screen");
    return server;
  }
  server->stop();  // no-op when the failed upgrade already stopped it
  MDNS.end();
  return {};
}

void stop() {
  setRemoteWanted(false);
  rejoinNow = false;
  rejoinSkippedForMemory = false;
  if (!remoteServer && !rejoining && !joinPending) return;
  stopServerAndRadioInBackground();
  LOG_INF("GDY", "wifi remote off");
}

void startInBackground() {
  stop();                  // a server left behind when the link dropped; no-op otherwise
  shutdownQueued = false;  // On supersedes an Off still queued behind a join
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
  if (!streaming && millis() - lastStreamMs > KNOBS.otaPulseGraceMs) {
    light.end();
    lit = false;
  }
}

void loop(const uint32_t idleMs) {
  updateOtaLight();
  if (shutdownQueued && !radioTask.running()) {
    shutdownQueued = false;
    stopServerAndRadioInBackground();
  }
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
    // A screen that left the link up and never took the server (OPDS, KOSync,
    // a File Transfer backed out of the Wi-Fi picker): the remote served on it
    // throughout, so nothing to restart. Same IP: same interface setup.
    const bool sharedLinkKept = owner == RadioOwner::None && remoteServer && WiFi.getMode() == WIFI_MODE_STA &&
                                hasActiveStationWifiConnection() && remoteIp == WiFi.localIP().toString().c_str();
    if (left != RadioOwner::None && owner != RadioOwner::Screen && !sharedLinkKept) {
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
    if (!remoteServer && !sharedStartTried && hasActiveStationWifiConnection() && hasInternalHeapForRemote()) {
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
  if (!rejoinNow && !rejoinAfterPause) {
    if (millis() < REJOIN_BOOT_DELAY_MS || idleMs < REJOIN_IDLE_MS) return;
    if (rejoinAt != 0 && millis() - rejoinAt < rejoinRetryMs) return;
  }
  rejoinByUser = rejoinNow;
  rejoinNow = false;
  rejoinAfterPause = false;
  beginRejoin();
}
}  // namespace goodies_remote

namespace {
// Knob rows are debug-only English (ids as in Knobs.def, CMD:KNOB and
// knobs.json), like the display test names: no I18n strings in release builds.
constexpr int KNOB_RESET_ALL = -2;
constexpr int KNOB_DIM_LEVEL = -3;  // Flash Dim Level: the Display > Frontlight setting, not a knob
constexpr int MAX_KNOB_TABS = 8;
constexpr int KBD_TURBO = -4;  // Keyboard test: the Turbo Keyboard setting, not a knob
// Keyboard test: the knobs that change typing feel, under Turbo keyboard.
constexpr const char* KBD_TEST_KNOBS[] = {"kbdFrames",         "kbdHighlightDelayMs", "kbdTouchHoldMs",
                                          "kbdTouchDelHoldMs", "contactJumpPx",       "tapSlopPx"};

// Knob groups in Knobs.def order (rows of a group are contiguous): one tab each.
int knobGroups(const char* (&out)[MAX_KNOB_TABS]) {
  int n = 0;
  for (int i = 0; i < knobs::COUNT; ++i) {
    if (n > 0 && strcmp(out[n - 1], knobs::INFO[i].group) == 0) continue;
    if (n == MAX_KNOB_TABS) break;
    out[n++] = knobs::INFO[i].group;
  }
  return n;
}
const char* editUnit = "";  // unit of the knob being edited, for formatKnob

void formatKnob(const int value, char* buf, const size_t len) { snprintf(buf, len, "%d %s", value, editUnit); }

std::string turboRowValue() { return SETTINGS.turboKeyboard ? tr(STR_STATE_ON) : tr(STR_STATE_OFF); }

// The scratch text's tail, cut on a UTF-8 boundary, so the row stays one line.
// Ends in ">": the row opens the keyboard.
std::string kbdTestPreview(const std::string& text) {
  constexpr size_t MAX_BYTES = 24;
  if (text.empty()) return ">";
  if (text.size() <= MAX_BYTES) return text + " >";
  size_t start = text.size() - MAX_BYTES;
  while (start < text.size() && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80) ++start;
  return "..." + text.substr(start) + " >";
}

std::string dimLevelRowValue() { return std::to_string(SETTINGS.flashDuckDepth) + " %"; }

std::string knobRowValue(const int index) {
  char buf[32];
  const int32_t v = knobs::get(index);
  snprintf(buf, sizeof(buf), "%ld %s%s", static_cast<long>(v), knobs::INFO[index].unit,
           v != knobs::INFO[index].def ? " *" : "");
  return buf;
}
}  // namespace

GoodiesActivity::GoodiesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Goodies", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void GoodiesActivity::onEnter() {
  Activity::onEnter();
  applySharedUiTheme(app, uiTarget);
  goodies_remote::takePickerRequest();  // left over from a toggle made on an earlier visit
  app.on(ACTION_ROW, &GoodiesActivity::onRowEvent, this);
  app.on(ACTION_TAB, &GoodiesActivity::onTabEvent, this);
  app.on(ACTION_STEP, &GoodiesActivity::onStepEvent, this);
  app.setScreen(&GoodiesActivity::listScreen, this);
  showLevel(Level::Root);
}

void GoodiesActivity::showLevel(const Level next) {
  RenderLock lock(*this);
  level = next;
  tokenShownUntil = 0;
  entries.clear();
  if (level == Level::Root) {
    // ">" (as in Settings) marks a row that opens another menu or page.
    entries.push_back({tr(STR_DISPLAY_TEST), -1, {}, ">"});
    entries.push_back({tr(STR_WIFI_REMOTE), -1, {}, remoteRowValue()});
    entries.push_back({"API Token", -1, {}, tokenRowValue()});
    entries.push_back({"Knobs", -1, {}, ">"});
    entries.push_back({"Keyboard Test", -1, {}, ">"});
    entries.push_back({"Pin Monitor", -1, {}, PinMon::enabled() ? "On >" : "Off >"});
    remoteRowShown = remoteRowState();
#ifndef SIMULATOR
    entries.push_back({tr(STR_BATTERY_STATS), -1, {}, ">"});
#if CROSSDINK_PSRAM_LOG
    entries.push_back({"Sleep-reboot-log", -1, {}});
#endif
#endif
  } else if (level == Level::PinMon) {
    // Read when opened: the toggle, then per pin level, changes and wakes since boot.
    entries.push_back({"Monitor", -1, {}});
    for (size_t i = 0; i < PinMon::PIN_COUNT; ++i) {
      const PinMon::PinStat p = PinMon::stat(i);
      char label[12], value[48];
      snprintf(label, sizeof(label), "GPIO%u", p.gpio);
      snprintf(value, sizeof(value), "%s%s, %lu changes, %lu wakes", p.chatter ? "chatter off, " : "",
               p.level ? "high" : "low", static_cast<unsigned long>(p.changes), static_cast<unsigned long>(p.wakes));
      entries.push_back({label, -1, {}, value});
    }
  } else if (level == Level::KeyboardTest) {
    entries.push_back({"Type", -1, {}, kbdTestPreview(kbdTestText)});
    entries.push_back({tr(STR_TURBO_KEYBOARD), KBD_TURBO, {}, turboRowValue()});
    for (const char* id : KBD_TEST_KNOBS) {
      const int i = knobs::find(id);
      if (i >= 0) entries.push_back({id, i, {}, knobRowValue(i)});
    }
  } else if (level == Level::Knobs) {
    const char* groups[MAX_KNOB_TABS];
    const int tabs = knobGroups(groups);
    knobTab = std::clamp(knobTab, 0, tabs - 1);
    for (int i = 0; i < knobs::COUNT; ++i) {
      if (strcmp(knobs::INFO[i].group, groups[knobTab]) != 0) continue;
      if (strcmp(groups[knobTab], "Light") == 0 && entries.empty()) {
        entries.push_back({"flashDimLevel", KNOB_DIM_LEVEL, {}, dimLevelRowValue()});
      }
      entries.push_back({knobs::INFO[i].id, i, {}, knobRowValue(i)});
    }
    entries.push_back({"Reset All", KNOB_RESET_ALL, {}});
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
  // The page's Monitor row is a switch (tapping it toggles). The Root row opens the page, so it shows
  // the state as text with ">" like every other row that opens a page; a switch would drop the ">".
  if (level == Level::PinMon) {
    rowItems[0].toggle = true;
    rowItems[0].toggleChecked = PinMon::enabled();
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
    } else if (index == 1) {
      toggleRemote();
    } else if (index == TOKEN_ROW) {
      // First tap: the full token for 10 s (loop() hides it again). Second: a new PIN.
      if (tokenShownUntil == 0) {
        tokenShownUntil = millis() + 10000;
        setRowValue(TOKEN_ROW, tokenRowValue());
      } else {
        confirmNewPin();
      }
    } else if (index == KNOBS_ROW) {
      showLevel(Level::Knobs);
    } else if (index == KBD_TEST_ROW) {
      showLevel(Level::KeyboardTest);
    } else if (index == PINMON_ROW) {
      showLevel(Level::PinMon);
#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)
    } else if (index == SLEEP_REBOOT_ROW) {
      // The sleep path, then a restart instead of power-down: the PSRAM log survives it (/api/psram-log).
      startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, "Sleep-reboot-log?",
                                                                    "Runs the sleep path, then reboots."),
                             [this](const ActivityResult& result) {
                               mappedInput.suppressNextConfirmRelease();
                               if (result.isCancelled) {
                                 requestUpdate();
                                 return;
                               }
                               SleepLog::armSleepReboot();
                               enterDeepSleep();  // restarts, does not return
                             });
#endif
    } else {
#ifndef SIMULATOR
      startActivityForResult(std::make_unique<BatteryStatsActivity>(renderer, mappedInput),
                             [this](const ActivityResult&) {
                               mappedInput.suppressNextConfirmRelease();
                               requestUpdate();
                             });
#endif
    }
    return;
  }
  if (level == Level::PinMon) {
    // Monitor: switches now (PinMon arms or releases the pins), RAM only. Other rows: refresh.
    if (index == 0) PinMon::setEnabled(!PinMon::enabled());
    showLevel(Level::PinMon);
    return;
  }
  if (level == Level::KeyboardTest) {
    if (index == 0) {
      // A scratch field for trying typing feel and speed: kept while Goodies is open, never saved.
      startActivityForResult(
          std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, "Keyboard Test", kbdTestText),
          [this](const ActivityResult& result) {
            mappedInput.suppressNextConfirmRelease();
            if (!result.isCancelled) {
              kbdTestText = std::get<KeyboardResult>(result.data).text;
              setRowValue(0, kbdTestPreview(kbdTestText));
            }
            requestUpdate();
          });
    } else if (entries[index].builtIn == KBD_TURBO) {
      stepKbdTest(index, 1);
    }
    return;
  }
  if (level == Level::Knobs) {
    if (entries[index].builtIn >= 0) {
      openKnob(index);
    } else if (entries[index].builtIn == KNOB_DIM_LEVEL) {
      openDimLevel(index);
    } else if (entries[index].builtIn == KNOB_RESET_ALL) {
      confirmResetKnobs();
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
  if (goodies_remote::waitingForMemory()) return 3;
  return goodies_remote::wanted() ? 1 : 0;
}

// Applies at once (the SDK copies are pushed) and saves knobs.json.
void GoodiesActivity::openKnob(const int row) {
  const int index = entries[row].builtIn;
  const knobs::Info& k = knobs::INFO[index];
  editUnit = k.unit;
  const int largeStep = std::max<int32_t>(k.step, std::min<int32_t>(k.step * 10, (k.max - k.min) / 4));
  auto picker = std::make_unique<IntervalSelectionActivity>(
      renderer, mappedInput, "KnobEdit", StrId::STR_GOODIES, knobs::get(index), k.min, k.max, k.step, largeStep,
      StrId::STR_NONE_OPT, /*readerActivity=*/false, /*allowPowerAsConfirm=*/false,
      /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/false, StrId::STR_NONE_OPT,
      /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true, formatKnob,
      /*tapStep=*/k.step, /*useReaderSlider=*/true);
  picker->setTitle(k.id);
  startActivityForResult(std::move(picker), [this, row, index](const ActivityResult& result) {
    mappedInput.suppressNextConfirmRelease();
    if (!result.isCancelled) {
      knobs::set(index, std::get<IntervalResult>(result.data).value);
      RenderLock lock(*this);
      entries[row].value = knobRowValue(index);
      rowItems[row].value = entries[row].value.c_str();
    }
    requestUpdate();
  });
}

// Same slider and value as Display > Frontlight > Flash Dim Level.
void GoodiesActivity::openDimLevel(const int row) {
  constexpr int step = CrossPointSettings::FLASH_DUCK_DEPTH_STEP;
  startActivityForResult(
      std::make_unique<IntervalSelectionActivity>(
          renderer, mappedInput, "FlashDuckDepth", StrId::STR_FLASH_DUCK_DEPTH, SETTINGS.flashDuckDepth, 0,
          CrossPointSettings::FLASH_DUCK_DEPTH_MAX, step, step, StrId::STR_NONE_OPT, /*readerActivity=*/false,
          /*allowPowerAsConfirm=*/false, /*ignoreInitialConfirmRelease=*/false, /*showPercentValue=*/true,
          StrId::STR_NONE_OPT, /*overrideDisabledReaderTouchscreen=*/false, /*showTouchHeaderBackButton=*/true,
          /*valueFormatter=*/nullptr, /*tapStep=*/step, /*useReaderSlider=*/true),
      [this, row](const ActivityResult& result) {
        mappedInput.suppressNextConfirmRelease();
        if (!result.isCancelled) {
          SETTINGS.flashDuckDepth = static_cast<uint8_t>(std::get<IntervalResult>(result.data).value);
          SETTINGS.saveToFile();
          RenderLock lock(*this);
          entries[row].value = dimLevelRowValue();
          rowItems[row].value = entries[row].value.c_str();
        }
        requestUpdate();
      });
}

void GoodiesActivity::confirmResetKnobs() {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, std::string(tr(STR_CONFIRM)) + ": Reset All Knobs",
                                             "Defaults for every knob; knobs.json is deleted."),
      [this](const ActivityResult& result) {
        mappedInput.suppressNextConfirmRelease();
        if (!result.isCancelled) {
          knobs::resetAll();
          RenderLock lock(*this);
          for (size_t row = 0; row < entries.size(); ++row) {
            if (entries[row].builtIn < 0) continue;
            entries[row].value = knobRowValue(entries[row].builtIn);
            rowItems[row].value = entries[row].value.c_str();
          }
        }
        requestUpdate();
      });
}

std::string GoodiesActivity::remoteRowValue() {
  switch (remoteRowState()) {
    case 2:
      // The state stays readable next to the address: "ON 10.0.1.67".
      return std::string(tr(STR_STATE_ON)) + " " + remoteIp;
    case 1:
      return tr(STR_CONNECTING);
    case 3:
      return tr(STR_MEMORY_ERROR);
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
  remoteRowShown = remoteRowState();
  setRowValue(1, remoteRowValue());
}

void GoodiesActivity::setRowValue(const int row, std::string value) {
  RenderLock lock(*this);
  entries[row].value = std::move(value);
  rowItems[row].value = entries[row].value.c_str();
  lock.unlock();
  requestUpdate();
}

void GoodiesActivity::confirmNewPin() {
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, "New API PIN?",
                                                                "Clients with the old PIN or token get locked out."),
                         [this](const ActivityResult& result) {
                           mappedInput.suppressNextConfirmRelease();
                           if (!result.isCancelled) {
                             char pin[SerialRemote::TOKEN_BUF];
                             SerialRemote::newPin(pin);
                             memset(pin, 0, sizeof(pin));
                             tokenShownUntil = millis() + 10000;
                           }
                           setRowValue(TOKEN_ROW, tokenRowValue());
                         });
}

// /api/cmd, /api/screenshot and /api/ota token (/debug/remote-token on the SD
// card). Missing or empty: the device makes a 6-digit PIN for clients to copy.
std::string GoodiesActivity::tokenRowValue() {
  char token[SerialRemote::TOKEN_BUF];
  size_t n = SerialRemote::readToken(token);
  if (n == 0) n = SerialRemote::newPin(token);
  std::string value = n == 0                 ? "none (SD write failed)"
                      : tokenShownUntil != 0 ? std::string(token) + "  (tap: new PIN)"
                      : n > 8                ? std::string("set ...") + (token + n - 4)
                                             : "tap to show";
  memset(token, 0, sizeof(token));
  return value;
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

void GoodiesActivity::onTabEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<GoodiesActivity*>(user);
  self->pendingTab = event.value;
  self->app.clearTapFlash();  // taps never highlight a tab
}

void GoodiesActivity::onStepEvent(const fui::ActionEvent& event, void* user) {
  static_cast<GoodiesActivity*>(user)->pendingStep = event.value;
}

// Applies at once; the write waits until the page is left (holdsSettingsFlush),
// or for sleep, restart or a firmware flash.
void GoodiesActivity::stepKbdTest(const int row, const int dir) {
  if (row <= 0 || row >= static_cast<int>(entries.size())) return;
  const int index = entries[row].builtIn;
  if (index == KBD_TURBO) {
    SETTINGS.turboKeyboard = SETTINGS.turboKeyboard ? 0 : 1;
    SETTINGS.saveToFile();
    setRowValue(row, turboRowValue());
    return;
  }
  knobs::set(index, knobs::get(index) + dir * knobs::INFO[index].step);
  setRowValue(row, knobRowValue(index));
}

void GoodiesActivity::switchKnobTab(const int tab) {
  const char* groups[MAX_KNOB_TABS];
  const int tabs = knobGroups(groups);
  knobTab = (tab + tabs) % tabs;
  showLevel(Level::Knobs);
}

void GoodiesActivity::loop() {
  if (goodies_remote::takePickerRequest()) {
    openRemotePicker();
    return;
  }
  // The background join finishes (or drops) while this screen is open.
  if (level == Level::Root && entries.size() > 1 && remoteRowState() != remoteRowShown) refreshRemoteRow();
  if (tokenShownUntil != 0 && static_cast<long>(millis() - tokenShownUntil) >= 0) {
    tokenShownUntil = 0;
    setRowValue(TOKEN_ROW, tokenRowValue());
  }
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
      pendingTab = -1;
      pendingStep = -1;
      const auto event = app.route(snapshot);
      if (app.invalidated()) requestUpdate();
      if (pendingTab >= 0) switchKnobTab(pendingTab);
      if (pendingStep >= 0) stepKbdTest(pendingStep / 2, pendingStep % 2 ? 1 : -1);
      if (pendingRow >= 0) activate(pendingRow);
      if (event) return;
    }
  }
  const int count = static_cast<int>(entries.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate(selectedIndex);
    return;
  }

  // Keyboard test: Up/Down pick a row, Left/Right are - / +.
  if (level == Level::KeyboardTest) {
    using Button = MappedInputManager::Button;
    if (mappedInput.wasReleased(Button::Left) || mappedInput.wasReleased(Button::Right)) {
      stepKbdTest(selectedIndex, mappedInput.wasReleased(Button::Right) ? 1 : -1);
    } else if (mappedInput.wasReleased(Button::Up) || mappedInput.wasReleased(Button::Down)) {
      selectedIndex = mappedInput.wasReleased(Button::Down) ? ButtonNavigator::nextIndex(selectedIndex, count)
                                                            : ButtonNavigator::previousIndex(selectedIndex, count);
      requestUpdate();
    }
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
  // Knobs: long-press Up/Down switches tabs, as in Settings; Up/Down stay in the list.
  if (level == Level::Knobs) {
    buttonNavigator.onNextContinuous([this] { switchKnobTab(knobTab + 1); });
    buttonNavigator.onPreviousContinuous([this] { switchKnobTab(knobTab - 1); });
    return;
  }
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

  if (level == Level::Knobs) {
    // Settings' tab bar: plain labels, the active tab underlined 3 px.
    const char* groups[MAX_KNOB_TABS];
    const int tabCount = knobGroups(groups);
    fui::TabItem tabs[MAX_KNOB_TABS];
    for (int i = 0; i < tabCount; ++i) {
      tabs[i].label = groups[i];
      tabs[i].value = static_cast<int16_t>(i);
      tabs[i].selected = i == knobTab;
    }
    fui::TabBarProps tabProps;
    tabProps.tabs = tabs;
    tabProps.count = static_cast<uint8_t>(tabCount);
    tabProps.action = ACTION_TAB;
    tabProps.inputMask = fui::InputTouch;
    tabProps.text = screen.theme().smallText;
    tabProps.tabInset = fui::Insets{2, 2, 4, 2};
    tabProps.contentInset = fui::Insets{2, 4, 2, 4};
    tabProps.divider = true;
    if (metrics.tabBarAppearance != ThemeTabBarAppearance::BorderedText) {
      tabProps.tabStyles = fui::plainStyles();
      tabProps.selectedUnderline = 3;
    }
    const int16_t lineHeight = screen.target().lineHeight(screen.theme().smallText.font);
    const int16_t band = std::max<int16_t>(mappedInput.hasTouch() ? 50 : static_cast<int16_t>(metrics.tabBarHeight),
                                           static_cast<int16_t>(lineHeight + 10));
    drawUiTabBar(screen, tabProps, screen.takeTop(band), metrics.tabBarAppearance);
    screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  }

  if (level == Level::KeyboardTest) {
    buildKbdTestRows(screen);
    return;
  }

  const int count = static_cast<int>(rowItems.size());
  fui::ListProps props;
  props.items = rowItems.data();
  props.count = static_cast<uint16_t>(count);
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;  // air between the value and the row edge, as in Settings
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, count);
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

// The Goodies list rows (configureUiList + the theme's list styles), laid out
// one by one so the knob rows can carry - / + controls.
void GoodiesActivity::buildKbdTestRows(UiApp::ScreenType& screen) {
  fui::ListProps list;
  list.labelText = screen.theme().bodyText;
  configureUiList(list, screen.theme(), screen.body());
  list = screen.resolveListProps(list);
  const int16_t inset = list.rowInset < 0 ? 0 : list.rowInset;
  const int16_t gap = list.rowGap < 0 ? 0 : list.rowGap;
  for (int row = 0; row < static_cast<int>(entries.size()); ++row) {
    fui::Rect rect = screen.takeTop(list.rowHeight, gap);
    // ponytail: no scrolling, the 9 rows fit portrait; add a ListNav if the page grows.
    if (rect.height < list.rowHeight) break;
    rect.x = static_cast<int16_t>(rect.x + inset);
    rect.width = static_cast<int16_t>(rect.width - inset * 2);
    if (row > 0 && list.separatorPaint.kind != fui::PaintKind::None) {
      screen.target().fill(fui::Rect{static_cast<int16_t>(rect.x + list.sidePadding),
                                     static_cast<int16_t>(rect.y - (gap > 1 ? gap / 2 : 1)),
                                     static_cast<int16_t>(rect.width - list.sidePadding * 2), 1},
                           list.separatorPaint);
    }
    fui::SettingRowProps props;
    props.label = entries[row].label.c_str();
    props.labelText = list.labelText;
    props.valueText = list.valueText;
    props.styles = list.rowStyles;
    props.radius = list.rowRadius;
    props.sidePadding = list.sidePadding;
    props.inputMask = fui::InputTouch;
    props.state = !list.hideSelection && row == selectedIndex ? fui::StateSelected : fui::StateNormal;
    if (row == 0) {
      props.value = entries[row].value.c_str();
      props.action = ACTION_ROW;
      props.valueId = 0;
      fui::settingRow(screen.frame(), rect, props);
      continue;
    }
    fui::StepperRowProps step;
    step.row = props;
    step.value = entries[row].value.c_str();
    step.widestValue = "8888 ms *";
    step.buttonStyles = fui::defaultButtonStyles();
    step.decrement = ACTION_STEP;
    step.increment = ACTION_STEP;
    step.decrementValue = static_cast<int16_t>(row * 2);
    step.incrementValue = static_cast<int16_t>(row * 2 + 1);
    fui::stepperRow(screen.frame(), rect, step);
  }
  visibleRows = static_cast<int>(entries.size());
}

void GoodiesActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const char* title = level == Level::Root           ? tr(STR_GOODIES)
                      : level == Level::Knobs        ? "Knobs"
                      : level == Level::KeyboardTest ? "Keyboard Test"
                      : level == Level::PinMon       ? "Pin Monitor"
                                                     : tr(STR_DISPLAY_TEST);
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  TouchHeaderBackButton::draw(renderer, uiTarget, header, title, false);
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

#endif  // CROSSDINK_GOODIES
