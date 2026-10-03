#include "WifiBackgroundJoin.h"

#ifndef SIMULATOR
#include <Arduino.h>
#include <Knobs.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

#include "SilentRestart.h"
#include "WifiCredentialStore.h"
#include "network/NetworkName.h"
#include "network/WifiUtils.h"
#include "util/WorkerTask.h"
#if CROSSDINK_GOODIES
#include "activities/goodies/GoodiesActivity.h"
#endif

namespace {
constexpr uint32_t TASK_STACK_BYTES = 6144;  // as the Goodies remote's join task: a wifi.json read and WiFi.begin()
// The same gate as entering a Wi-Fi screen in place: the driver's buffers are internal RAM.
KNOB_ALIAS(MIN_INTERNAL_FREE, netEntryMinFree);  // Goodies > Knobs
KNOB_ALIAS(MIN_INTERNAL_BLOCK, netEntryMinBlock);
KNOB_ALIAS(JOIN_TIMEOUT_MS, autoConnectTimeoutMs);  // as the Wi-Fi screen's saved-network join

WorkerTask task;                 // one join or teardown at a time; static so it outlives the screen that started it
std::atomic<bool> begun{false};  // the join task ran WiFi.begin()
uint32_t startedAt = 0;          // millis() of start(); the join timeout runs from here

void joinTask(void*) {
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) {
    LOG_INF("WIFI", "Background join: no saved network");
    return;
  }
  WiFi.persistent(false);  // credentials live in WifiCredentialStore, not NVS
  if (!WiFi.mode(WIFI_STA)) {
    LOG_ERR("WIFI", "Background join: station mode failed");
    return;
  }
  // As the Wi-Fi screen: strongest matching AP, and a hostname routers can show.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  WiFi.setHostname((String(NET_AP_SSID) + "-" + mac).c_str());
  // Modem sleep stays on (its default): nothing here turns it back on after the
  // join, and a slow DHCP answer costs nothing in the background.
  WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
  begun.store(true, std::memory_order_relaxed);
  LOG_INF("WIFI", "Background join of %s begun (%lu ms)", cred->ssid.c_str(),
          static_cast<unsigned long>(millis() - startedAt));
}

void offTask(void*) {
  WiFi.disconnect(false);
  WiFi.setSleep(true);
  WiFi.mode(WIFI_OFF);
  LOG_INF("WIFI", "Background join: radio off");
}
}  // namespace

namespace wifi_background_join {
void start() {
  wait();  // a teardown still running (Back and straight in again)
  begun.store(false, std::memory_order_relaxed);
  if (WiFi.getMode() != WIFI_MODE_NULL) return;  // connected, joining, or the remote's link
  const size_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (internalFree < MIN_INTERNAL_FREE || internalLargest < MIN_INTERNAL_BLOCK) {
    LOG_INF("WIFI", "Background join skipped: internal free %u largest %u", static_cast<unsigned>(internalFree),
            static_cast<unsigned>(internalLargest));
    return;
  }
  startedAt = millis();
  // Priority 1, under the main loop: it runs only while the loop waits.
  if (!task.start(joinTask, nullptr, TASK_STACK_BYTES, "WifiJoin"))
    LOG_ERR("WIFI", "Background join task did not start");
}

void stop() {
  wait();
  begun.store(false, std::memory_order_relaxed);
  if (WiFi.getMode() == WIFI_MODE_NULL) return;
#if CROSSDINK_GOODIES
  // The remote adopts the link (or drops it) from its own loop.
  if (goodies_remote::wanted()) return;
#endif
  if (!task.start(offTask, nullptr, TASK_STACK_BYTES, "WifiOff")) offTask(nullptr);
}

void wait() {
  while (task.running()) vTaskDelay(1);
}

bool joining() {
  if (task.running()) return true;
  return begun.load(std::memory_order_relaxed) && !hasActiveStationWifiConnection() &&
         millis() - startedAt < JOIN_TIMEOUT_MS;
}
}  // namespace wifi_background_join

#else  // SIMULATOR: no background network jobs.

namespace wifi_background_join {
void start() {}
void stop() {}
void wait() {}
bool joining() { return false; }
}  // namespace wifi_background_join

#endif
