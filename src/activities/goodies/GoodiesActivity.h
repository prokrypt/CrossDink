#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class CrossPointWebServer;

// Background Wi-Fi remote (Goodies > Wi-Fi remote): a log-only web server
// that outlives Goodies, so /api/psram-log answers while other screens run.
// Main task only.
namespace goodies_remote {
bool running();
// The toggle (SETTINGS.goodiesWifiRemote): on, whether or not connected yet.
bool wanted();
// Wanted, but the last rejoin was skipped for internal RAM; it retries on the backoff.
bool waitingForMemory();
// Toggle on without a Wi-Fi screen: joins the saved network in the background.
// With no saved network the toggle turns back off and takePickerRequest() is
// set once, so Goodies can open the Wi-Fi picker.
void startInBackground();
bool takePickerRequest();
// Blocks until a running join or toggle-off task is done. Called before a Wi-Fi screen's
// onEnter() and before deep sleep, so no Wi-Fi call overlaps the task's.
void waitForJoin();
// Toggle off: server and Wi-Fi off, forgets the toggle.
void stop();
// A screen needs port 80 and the radio: server and Wi-Fi off, the toggle stays.
// keepStation: the screen joins as a station (not AP), so a link to the
// remote's own network stays up for it to reuse, and so does the server,
// for takeServer().
void pause(bool keepStation = false);
// A station screen that needs the full web server (File Transfer, Calibre):
// the remote's running server, upgraded in place (no socket closed), when the
// link is still the remote's network. Empty otherwise: begin a new server.
std::unique_ptr<CrossPointWebServer> takeServer();
// The remote is on and the radio is a station on the remote's network: a
// leaving Wi-Fi screen should hand the link back instead of tearing it down.
bool keepsStation();
// The idle server lets the main loop power save, as File Transfer's does.
bool allowsRadioIdleSleep();
// Main loop: once no Wi-Fi screen (Activity::usesWifi) is on the stack, or
// after any boot (restart, sleep wake, power-on), rejoins the last network and
// restarts the server. Joins wait 5 s after boot and for idleMs >= 2 s of no
// input; failures back off 1 to 10 min. Beside screens that share their link
// (sharesWifiWithRemote) it only starts the server. No-op while the toggle is off.
void loop(uint32_t idleMs);
}  // namespace goodies_remote

// Debug-build Goodies menu (CROSSDINK_GOODIES). Root lists the tools; the
// Display test level lists built-in tests, then /debug/display/*.txt.
class GoodiesActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<24, 4>;  // list rows + knob tabs

 public:
  GoodiesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Keyboard test: knob and Turbo changes wait to be written until the page is left.
  bool holdsSettingsFlush() const override { return level == Level::KeyboardTest; }

 private:
  enum class Level : uint8_t { Root, DisplayTests, Knobs, KeyboardTest, PinMon };
  // Root: Display test, Wi-Fi remote, API token, Knobs, Keyboard test, Pin monitor, Battery & stats
  static constexpr int TOKEN_ROW = 2;
  static constexpr int KNOBS_ROW = 3;
  static constexpr int KBD_TEST_ROW = 4;
  static constexpr int PINMON_ROW = 5;
  static constexpr int SLEEP_REBOOT_ROW = 7;  // after Battery Stats (6), debug builds with the PSRAM log
  struct Entry {
    std::string label;
    int builtIn;       // Display tests: >= 0 display_script::BUILT_INS index. Knobs: knob index, < 0 Reset all
    std::string path;  // SD script when builtIn < 0
    std::string value = {};
  };

  Level level = Level::Root;
  std::vector<Entry> entries;
  std::vector<freeink::ui::ListItem> rowItems;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int pendingRow = -1;
  int visibleRows = 1;
  int topIndex = 0;
  int knobTab = 0;  // Knobs: one tab per Knobs.def group
  int pendingTab = -1;
  int pendingStep = -1;
  std::string kbdTestText;  // Keyboard test scratch text; never saved
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};

  void showLevel(Level next);
  void activate(int index);
  int remoteRowShown = -1;
  unsigned long tokenShownUntil = 0;  // millis(); 0 = API token row shows only its last 4 characters

  void toggleRemote();
  void openKnob(int row);
  void openDimLevel(int row);
  void confirmResetKnobs();
  void refreshRemoteRow();
  void setRowValue(int row, std::string value);
  std::string tokenRowValue();
  void confirmNewPin();
  void openRemotePicker();
  static int remoteRowState();
  static std::string remoteRowValue();
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTabEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  void stepKbdTest(int row, int dir);
  void buildKbdTestRows(UiApp::ScreenType& screen);
  void switchKnobTab(int tab);
  void buildListScreen(UiApp::ScreenType& screen);
};
