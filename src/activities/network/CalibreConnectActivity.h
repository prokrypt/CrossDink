#pragma once

#include <functional>
#include <memory>
#include <string>

#include "activities/Activity.h"
#include "activities/ScreenTransitionRefresh.h"
#include "network/CrossPointWebServer.h"
#include "util/TransferLightPulse.h"

enum class CalibreConnectState { WIFI_SELECTION, SERVER_STARTING, SERVER_RUNNING, ERROR };

/**
 * CalibreConnectActivity starts the file transfer server in STA mode,
 * but renders Calibre-specific instructions instead of the web transfer UI.
 */
class CalibreConnectActivity final : public Activity {
  CalibreConnectState state = CalibreConnectState::WIFI_SELECTION;
  ScreenTransitionRefresh screenTransitionRefresh;

  std::unique_ptr<CrossPointWebServer> webServer;
  TransferLightPulse transferLight;
  std::string connectedIP;
  std::string connectedSSID;
  size_t lastProgressReceived = 0;
  size_t lastProgressTotal = 0;
  std::string currentUploadName;
  std::string lastCompleteName;
  unsigned long lastCompleteAt = 0;
  unsigned long lastProcessedCompleteAt = 0;  // Track which server value we've already processed
  bool exitRequested = false;
  bool returnToReader = false;

  void renderServerRunning() const;

  void onWifiSelectionComplete(bool connected);
  void startWebServer();
  void stopWebServer();

 public:
  explicit CalibreConnectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool returnToReader = false)
      : Activity("CalibreConnect", renderer, mappedInput), returnToReader(returnToReader) {}
  void onEnter() override;
  void onExit() override;
  bool usesWifi() const override { return true; }
  void loop() override;
  void render(RenderLock&&) override;
  // Same power policy as File Transfer.
  bool allowsRadioIdleSleep() override {
    return webServer && webServer->allowsIdleSleep() && !webServer->isTransferActive();
  }
  bool powerOffPanelWhenIdle() const override { return true; }
  bool preventAutoSleep() override { return webServer && webServer->isRunning(); }
};
