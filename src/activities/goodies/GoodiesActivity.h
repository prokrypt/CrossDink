#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

// Background Wi-Fi remote (Goodies > Wi-Fi remote): a log-only web server
// that outlives Goodies, so /api/psram-log answers while other screens run.
// Main task only.
namespace goodies_remote {
bool running();
// The toggle (SETTINGS.goodiesWifiRemote): on, whether or not connected yet.
bool wanted();
// Toggle on without a Wi-Fi screen: joins the saved network in the background.
// False (nothing changed) when no network is saved.
bool startInBackground();
// Toggle off: server and Wi-Fi off, forgets the toggle.
void stop();
// A screen needs port 80 and the radio: server and Wi-Fi off, the toggle stays.
void pause();
// The idle server lets the main loop power save, as File Transfer's does.
bool allowsRadioIdleSleep();
// Main loop: once no Wi-Fi screen (Activity::usesWifi) is on the stack, or
// after any boot (restart, sleep wake, power-on), rejoins the last network and
// restarts the server. Joins wait 5 s after boot and for idleMs >= 2 s of no
// input; failures back off 1 to 10 min. No-op while the toggle is off.
void loop(uint32_t idleMs);
}  // namespace goodies_remote

// Debug-build Goodies menu (CROSSDINK_GOODIES). Root lists the tools; the
// Display test level lists built-in tests, then /debug/display/*.txt.
class GoodiesActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<16, 4>;

 public:
  GoodiesActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Level : uint8_t { Root, DisplayTests };
  struct Entry {
    std::string label;
    int builtIn;       // >= 0: display_script::BUILT_INS index
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
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};

  void showLevel(Level next);
  void activate(int index);
  int remoteRowShown = -1;

  void toggleRemote();
  static int remoteRowState();
  static std::string remoteRowValue();
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
};
