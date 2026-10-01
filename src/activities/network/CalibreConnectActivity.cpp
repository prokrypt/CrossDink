#include "CalibreConnectActivity.h"

#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <WiFi.h>

#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/goodies/GoodiesActivity.h"
#include "components/CompactHeader.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkName.h"
#include "util/BatteryLog.h"

namespace {
constexpr const char* HOSTNAME = NET_HOSTNAME;
}  // namespace

void CalibreConnectActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::event("xfer_start", "calibre");
#if CROSSDINK_GOODIES
  // Port 80 passes to this screen's own server; a link to the remote's network stays.
  goodies_remote::pause(/*keepStation=*/true);
#endif
  sdFontSystem.releaseLoadedFont(renderer);

  requestUpdate();
  state = CalibreConnectState::WIFI_SELECTION;
  connectedIP.clear();
  connectedSSID.clear();
  lastProgressReceived = 0;
  lastProgressTotal = 0;
  currentUploadName.clear();
  lastCompleteName.clear();
  lastCompleteAt = 0;
  lastProcessedCompleteAt = 0;
  exitRequested = false;

  if (WiFi.status() != WL_CONNECTED) {
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    connectedIP = WiFi.localIP().toString().c_str();
    connectedSSID = WiFi.SSID().c_str();
    startWebServer();
  }
}

void CalibreConnectActivity::onExit() {
  BatteryLog::event("xfer_end", "calibre");
  library::invalidateLibraryIndex();
  Activity::onExit();
  transferLight.end();

  // Wi-Fi belongs to the parent File Transfer screen, which leaves it (in
  // place or by restart) on its own exit.
  stopWebServer();
  MDNS.end();
}

void CalibreConnectActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    finish();
    return;
  }

  startWebServer();
}

void CalibreConnectActivity::startWebServer() {
  state = CalibreConnectState::SERVER_STARTING;
  requestUpdate();

  MDNS.end();
  if (MDNS.begin(HOSTNAME)) {
    // mDNS is optional for the Calibre plugin but still helpful for users.
    LOG_DBG("CAL", "mDNS started: http://%s.local/", HOSTNAME);
  }

#if CROSSDINK_GOODIES
  webServer = goodies_remote::takeServer();  // the running remote's server, no socket closed
#endif
  if (!webServer) {
    webServer.reset(new CrossPointWebServer());
    webServer->begin();
  }

  if (webServer->isRunning()) {
    state = CalibreConnectState::SERVER_RUNNING;
    // Pulse only once the server is up; Wi-Fi selection keeps the user's brightness.
    transferLight.begin();
    requestUpdate();
  } else {
    state = CalibreConnectState::ERROR;
    requestUpdate();
  }
}

void CalibreConnectActivity::stopWebServer() {
  transferLight.end();  // restores the user's brightness
  if (webServer) {
    webServer->stop();
    webServer.reset();
  }
}

void CalibreConnectActivity::loop() {
  transferLight.update(webServer && webServer->isMovingData(TransferLightPulse::TAIL_MS));
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
      mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture()) {
    exitRequested = true;
  }

  if (webServer && webServer->isRunning()) {
    // Requests are served on the server's own task.
    const auto status = webServer->getWsUploadStatus();
    bool changed = false;
    if (status.inProgress) {
      if (status.received != lastProgressReceived || status.total != lastProgressTotal ||
          status.filename != currentUploadName) {
        lastProgressReceived = status.received;
        lastProgressTotal = status.total;
        currentUploadName = status.filename;
        changed = true;
      }
    } else if (lastProgressReceived != 0 || lastProgressTotal != 0) {
      lastProgressReceived = 0;
      lastProgressTotal = 0;
      currentUploadName.clear();
      changed = true;
    }
    // Only update lastCompleteAt if the server has a NEW value (not one we already processed)
    // This prevents restoring an old value after the 6s timeout clears it
    if (status.lastCompleteAt != 0 && status.lastCompleteAt != lastProcessedCompleteAt) {
      lastCompleteAt = status.lastCompleteAt;
      lastCompleteName = status.lastCompleteName;
      lastProcessedCompleteAt = status.lastCompleteAt;  // Mark this value as processed
      changed = true;
    }
    if (lastCompleteAt > 0 && (millis() - lastCompleteAt) >= 6000) {
      lastCompleteAt = 0;
      lastCompleteName.clear();
      // Note: we DON'T reset lastProcessedCompleteAt here, so we won't re-process the old server value
      changed = true;
    }
    if (changed) {
      requestUpdate();
    }
  }

  if (exitRequested) {
    finish();
    return;
  }
}

void CalibreConnectActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  TouchHeaderBackButton::drawCompact(renderer, tr(STR_CALIBRE_WIRELESS));
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height) / 2;

  if (state == CalibreConnectState::SERVER_STARTING) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CALIBRE_STARTING));
  } else if (state == CalibreConnectState::ERROR) {
    renderer.drawCenteredText(UI_12_FONT_ID, top, tr(STR_CONNECTION_FAILED), true, EpdFontFamily::BOLD);
  } else if (state == CalibreConnectState::SERVER_RUNNING) {
    const int subHeaderTop = CompactHeader::contentTop(metrics);
    GUI.drawSubHeader(renderer, Rect{0, subHeaderTop, pageWidth, metrics.tabBarHeight}, connectedSSID.c_str());

    // Keep the network name and full address independently readable on narrow
    // screens. Sharing one subheader row forces one of them to be truncated.
    const std::string ipLabel = std::string(tr(STR_IP_ADDRESS_PREFIX)) + connectedIP;
    const int ipTop = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, ipTop, ipLabel.c_str());

    int y = ipTop + height + metrics.verticalSpacing * 3;
    const auto heightText12 = renderer.getTextHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_SETUP), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_INSTRUCTION_1));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height, tr(STR_CALIBRE_INSTRUCTION_2));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height * 2, tr(STR_CALIBRE_INSTRUCTION_3));
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y + height * 3, tr(STR_CALIBRE_INSTRUCTION_4));

    y += height * 3 + metrics.verticalSpacing * 4;
    renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding, y, tr(STR_CALIBRE_STATUS), true, EpdFontFamily::BOLD);
    y += heightText12 + metrics.verticalSpacing * 2;

    const bool showUploadProgress = lastProgressTotal > 0 && lastProgressReceived <= lastProgressTotal;
    if (showUploadProgress) {
      std::string label = tr(STR_CALIBRE_RECEIVING);
      if (!currentUploadName.empty()) {
        label += ": " + currentUploadName;
        label = renderer.truncatedText(SMALL_FONT_ID, label.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                       EpdFontFamily::REGULAR);
      }
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, label.c_str());
      GUI.drawProgressBar(renderer,
                          Rect{metrics.contentSidePadding, y + height + metrics.verticalSpacing,
                               pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                          lastProgressReceived, lastProgressTotal);
      y += height + metrics.verticalSpacing * 2 + metrics.progressBarHeight;
    }

    if (!showUploadProgress && lastCompleteAt > 0 && (millis() - lastCompleteAt) < 6000) {
      std::string msg = std::string(tr(STR_CALIBRE_RECEIVED)) + lastCompleteName;
      msg = renderer.truncatedText(SMALL_FONT_ID, msg.c_str(), pageWidth - metrics.contentSidePadding * 2,
                                   EpdFontFamily::REGULAR);
      renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding, y, msg.c_str());
    }

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
}
