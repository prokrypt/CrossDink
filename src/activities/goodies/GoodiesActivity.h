#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

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
  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
};
