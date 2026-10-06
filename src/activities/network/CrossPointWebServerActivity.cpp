#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Memory.h>
#include <WiFi.h>

#include <cstddef>

#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/ActivityManager.h"
#include "activities/goodies/GoodiesActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/NetworkName.h"
#include "network/WifiUtils.h"
#include "util/BatteryLog.h"
#include "util/QrUtils.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = NET_AP_SSID;
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = NET_HOSTNAME;
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();
  BatteryLog::event("xfer_start", "file-transfer");
  radioTaken = false;
  enteredUiTheme = SETTINGS.uiTheme;
  enteredUiScale = SETTINGS.uiScale;
  // Build or refresh the compact on-disk font index before Wi-Fi starts. The
  // C3 has substantially more contiguous heap here than while serving HTTP.
  // The in-place relaunch after the mode picker follows a scan made moments
  // ago on the previous entry, so it skips a second one.
  constexpr uint32_t REGISTRY_FRESH_MS = 30000;
  if (!networkBootReady || !sdFontSystem.registryRefreshedWithin(REGISTRY_FRESH_MS)) {
    sdFontSystem.ensureRegistry();
  }
  sdFontSystem.releaseForNetwork(renderer);

  LOG_DBG("WEBACT", "Free heap at onEnter: %" PRId32 " bytes", ESP.getFreeHeap());

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  requestUpdate();

  if (hasInitialNetworkMode) {
    onNetworkModeSelected(initialNetworkMode);
    return;
  }

  // Launch network mode selection subactivity
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             exitToOrigin();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  BatteryLog::event("xfer_end", "file-transfer");
  library::invalidateLibraryIndex();
  Activity::onExit();
  transferLight.end();

  state = WebServerActivityState::SHUTTING_DOWN;
  // Picker left without a mode: the remote kept the radio and port 80.
  if (!radioTaken && !networkBootReady) return;

  const bool wifiWasActive = WiFi.getMode() != WIFI_MODE_NULL;

  stopDnsServer();
  MDNS.end();

  // Stop local services before disconnecting/restarting WiFi.
  stopWebServer();
  MDNS.end();
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
  // Let sockets close before Wi-Fi goes down; nothing to wait for when the
  // exit comes straight from the mode picker (no radio, no services).
  if (wifiWasActive) delay(50);

  // Wi-Fi goes down after local services have released their sockets. A
  // session that left the internal heap too fragmented, or changed the UI
  // theme or scale from the portal, still reboots.
  const bool uiChanged = SETTINGS.uiTheme != enteredUiTheme || SETTINGS.uiScale != enteredUiScale;
  if ((wifiWasActive || networkBootReady) && (uiChanged || !leaveNetworkInPlace())) {
    if (returnBookPath.empty()) {
      silentRestart();
    } else {
      silentRestartToReader();
    }
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %" PRId32 " bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
#if CROSSDINK_GOODIES
  // Port 80 and the radio pass to this screen's own server. A rejoin may have
  // started while the picker was up; it must end before the radio goes off.
  goodies_remote::waitForJoin();
  goodies_remote::pause(/*keepStation=*/mode != NetworkMode::CREATE_HOTSPOT);
#endif
  radioTaken = true;
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  } else if (mode == NetworkMode::NEARBY_STATS_SYNC) {
    modeName = "Sync Stats";
  } else if (mode == NetworkMode::NEARBY_BOOK_RECEIVE) {
    modeName = "Receive File";
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  if (mode == NetworkMode::USB_DRIVE) {
    activityManager.goToUsbDrive();
    return;
  }

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::NEARBY_STATS_SYNC) {
    activityManager.goToNearbyStatsSync();
    return;
  }
  if (mode == NetworkMode::NEARBY_BOOK_RECEIVE) {
    activityManager.goToNearbyBookReceive();
    return;
  }

  if (!networkBootReady) {
    switch (mode) {
      case NetworkMode::JOIN_NETWORK:
        activityManager.goToJoinNetworkFileTransfer(returnBookPath);
        break;
      case NetworkMode::CONNECT_CALIBRE:
        activityManager.goToCalibreWireless(returnBookPath);
        break;
      case NetworkMode::CREATE_HOTSPOT:
        activityManager.goToHotspotFileTransfer(returnBookPath);
        break;
      case NetworkMode::USB_DRIVE:
        activityManager.goToUsbDrive();
        break;
      case NetworkMode::NEARBY_STATS_SYNC:
      case NetworkMode::NEARBY_BOOK_RECEIVE:
        break;
    }
    return;
  }

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    // The child activity must survive this callback; allocate only its small control object on the heap.
    auto calibreActivity = makeUniqueNoThrow<CalibreConnectActivity>(renderer, mappedInput, !returnBookPath.empty());
    if (!calibreActivity) {
      LOG_ERR("WEBACT", "OOM: Calibre activity (size=%u free=%" PRIu32 " maxAlloc=%" PRIu32 ")",
              static_cast<unsigned>(sizeof(CalibreConnectActivity)), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      exitToOrigin();
      return;
    }

    startActivityForResult(std::move(calibreActivity), [this](const ActivityResult& result) {
      state = WebServerActivityState::MODE_SELECTION;

      if (networkBootReady) {
        exitToOrigin();
        return;
      }

      startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                             [this](const ActivityResult& result) {
                               if (result.isCancelled) {
                                 exitToOrigin();
                               } else {
                                 onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                               }
                             });
    });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    WiFi.mode(WIFI_STA);

    state = WebServerActivityState::WIFI_SELECTION;
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
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdate();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Start mDNS for hostname resolution
    restartMdns(AP_HOSTNAME, "WEBACT");

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               exitToOrigin();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Free heap before AP start: %" PRId32 " bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    exitToOrigin();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  // Start mDNS for hostname resolution
  restartMdns(AP_HOSTNAME, "WEBACT");

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  dnsServer = new DNSServer();
  dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer->start(DNS_PORT, "*", apIP);

  LOG_DBG("WEBACT", "Free heap after AP start: %" PRId32 " bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  // Create the web server instance
#if CROSSDINK_GOODIES
  webServer = goodies_remote::takeServer();  // the running remote's server, no socket closed
#endif
  if (!webServer) {
    webServer.reset(new CrossPointWebServer());
    webServer->begin();
  }

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    // The pulse (and its 0% idle level) starts only once the server is up, so
    // the mode menu and Wi-Fi picker keep the user's brightness.
    transferLight.begin();
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    exitToOrigin();
  }
}

void CrossPointWebServerActivity::exitToOrigin() {
  // POST /api/exit?flash=... : reboot into SD Card Firmware Update for that file.
  // Returns only when deep sleep superseded the reboot; then exit normally.
  if (webServer) {
    const std::string flashPath = webServer->takeExitFlashPath();
    if (!flashPath.empty()) {
      silentRestartToFirmwareUpdate(flashPath);
    }
  }

  if (returnBookPath.empty()) {
    onGoHome();
    return;
  }

  activityManager.goToReader(returnBookPath, true);
}

void CrossPointWebServerActivity::stopWebServer() {
  transferLight.end();  // restores the user's brightness
  if (webServer && webServer->isRunning()) {
    webServer->stop();
  }
  webServer.reset();
}

void CrossPointWebServerActivity::loop() {
  transferLight.update(webServer && webServer->isMovingData(TransferLightPulse::TAIL_MS));
  if ((state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) &&
      exitRequested()) {
    exitToOrigin();
    return;
  }

  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          // Bar changes repaint this screen's own indicator, at most once per 5 s
          // (a repaint already due, e.g. reconnect, carries the new bars).
          static unsigned long lastBarsRepaint = 0;
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars && (repaint || millis() - lastBarsRepaint >= 5000)) {
            lastBarsRepaint = millis();
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }
  }
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    const auto pageHeight = renderer.getScreenHeight();

    if (state == WebServerActivityState::SERVER_RUNNING) {
      renderServerRunning();
    } else {
      renderHeader();
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }
    renderer.displayBuffer(screenTransitionRefresh.modeFor(static_cast<uint8_t>(state)));
  }
}

void CrossPointWebServerActivity::renderHeader() const {
  const char* title = isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER);
  TouchHeaderBackButton::draw(renderer, TouchHeaderBackButton::headerRect(renderer, mappedInput), title, false);
}

bool CrossPointWebServerActivity::exitRequested() const {
  return TouchHeaderBackButton::wasTapped(mappedInput, renderer) ||
         mappedInput.wasPressed(MappedInputManager::Button::Back) || mappedInput.wasHomeGesture() ||
         (webServer && webServer->consumeExitRequest());
}

namespace {
// Draws a QR code centred at y with its caption (and optional small caption)
// directly below it; returns the y just below the last caption.
int drawCenteredQr(const GfxRenderer& renderer, int y, const std::string& payload, const char* caption,
                   const char* smallCaption = nullptr) {
  const Rect bounds((renderer.getScreenWidth() - QR_CODE_WIDTH) / 2, y, QR_CODE_WIDTH, QR_CODE_HEIGHT);
  QrUtils::drawQrCode(renderer, bounds, payload);
  y += QR_CODE_HEIGHT + UITheme::getInstance().getMetrics().verticalSpacing;
  renderer.drawCenteredText(UI_10_FONT_ID, y, caption);
  y += renderer.getLineHeight(UI_10_FONT_ID);
  if (smallCaption) {
    renderer.drawCenteredText(SMALL_FONT_ID, y, smallCaption);
    y += renderer.getLineHeight(SMALL_FONT_ID);
  }
  return y;
}
}  // namespace

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  renderHeader();
  const int subHeaderTop = metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput);
  GUI.drawSubHeader(renderer, Rect{0, subHeaderTop, pageWidth, metrics.tabBarHeight}, connectedSSID.c_str());

  int startY = subHeaderTop + metrics.tabBarHeight + metrics.verticalSpacing;
  const int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_CONNECT_WIFI_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing;

    // Wi-Fi QR follows spec at
    // https://github.com/zxing/zxing/wiki/Barcode-Contents#wi-fi-network-config-android-ios-11
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    startY = drawCenteredQr(renderer, startY, wifiConfig, connectedSSID.c_str()) + metrics.verticalSpacing;

    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing;

    // Hostname URL first, IP address as fallback.
    const std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    const std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";
    drawCenteredQr(renderer, startY, hostnameUrl, hostnameUrl.c_str(), ipUrl.c_str());
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing;

    // IP URL first, hostname as fallback.
    const std::string webInfo = "http://" + connectedIP + "/";
    const std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    drawCenteredQr(renderer, startY, webInfo, webInfo.c_str(), hostnameUrl.c_str());
  }

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_EXIT)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}
