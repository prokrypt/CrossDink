#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <atomic>
#include <bitset>
#include <memory>

#include "OpdsServerStore.h"
#include "activities/Activity.h"
#include "activities/browser/OpdsPageCache.h"
#include "activities/browser/OpdsPreloadPool.h"
#include "components/OptionPopup.h"
#include "util/ButtonNavigator.h"

/**
 * Activity showing the list of configured OPDS servers.
 * Allows adding new servers and editing/deleting existing ones.
 * When pickerMode is true, selecting a server navigates to the OPDS browser
 * instead of opening the editor (used from the home screen).
 */
class OpdsServerListActivity final : public Activity {
 public:
  explicit OpdsServerListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool pickerMode = false);

  void onEnter() override;
  void onExit() override;
  // From Home, the list joins the saved Wi-Fi network in the background so the
  // browser finds the link up; the Goodies remote may serve on it meanwhile.
  bool usesWifi() const override { return pickerMode; }
  bool sharesWifiWithRemote() const override { return true; }
  // Once the join has settled, the idle list lets the loop power save with
  // Wi-Fi up, as the browser's list does.
  bool allowsRadioIdleSleep() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // FreeInkApp hosts the server list (themed rows, touch routing); the header
  // stays on GUI.drawHeader for the battery indicator.
  using UiApp = freeink::ui::FreeInkApp<20, 4>;

  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  bool pickerMode = false;
  // A server was picked: the browser takes over the background join's link
  // and the page cache.
  bool leavingToBrowser = false;
  // PSRAM devices, picker mode: each server's root page is fetched in the
  // background once Wi-Fi is up (one server at a time) and its row gets the
  // check mark; the cache goes to the browser with the picked server.
  std::unique_ptr<OpdsPageCache> pageCache;
  std::unique_ptr<OpdsPreloadPool> preload;  // the current server's fetch; destroyed before pageCache
  size_t prefetchIndex = 0;                  // next server to fetch
  uint32_t pageCachedAt = 0;                 // pageCache->changes() when rootCached was set
  // Servers whose root page is cached, set on the main loop under the render
  // lock (the cache has no lock) and read by the screen builder.
  std::bitset<OpdsServerStore::MAX_SERVERS> rootCached;
  OptionPopup optionPopup;

  freeink::ui::GfxRendererTarget uiTarget;  // must precede `app`: the app holds a reference to it
  UiApp app;
  // render() rebuilds the app's interaction table; loop() only routes touch
  // snapshots against it while this is true (the two run on different tasks).
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;  // rows per page at the current scale; set by the screen builder
  int topIndex = 0;     // viewport scroll position, decoupled from the selection

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);

  int getItemCount() const;
  void handleSelection();
  void pumpPrefetch();
};
