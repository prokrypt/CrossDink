#include "DeviceIdentity.h"

#include <Logging.h>

#include <cstdio>
#include <cstring>

#if !defined(SIMULATOR) && LOG_LEVEL >= 2
#include <Arduino.h>
#include <BoardConfig.h>
#include <XteinkDetect.h>
#include <esp_heap_caps.h>

#include "AppVersion.h"
#include "BuildInfo.h"
#include "DeviceSecurity.h"
#endif

namespace DeviceIdentity {

#if !defined(SIMULATOR) && LOG_LEVEL >= 2

namespace {
using BoardConfig::DisplayController;

const char* detectMethod(const freeink::XteinkDisplayProbeDiag& d) {
  if (d.valid && d.verBytesRead >= 5) return "bus probe (VER/FLG)";
  if (d.valid && d.verBytesRead > 0) return "X3 VER probe";
  if (d.promoted) return "NVS screenType";
  return "board profile";
}

// MTP 0x017-0x019 product id and 0x01A-0x027 LUT version, as hex; "-" when the
// probe did not read the MTP.
void formatPanelId(const freeink::XteinkDisplayProbeDiag& d, char* id, size_t idSize, char* lut, size_t lutSize) {
  if (!d.mtpValid) {
    snprintf(id, idSize, "-");
    snprintf(lut, lutSize, "-");
    return;
  }
  snprintf(id, idSize, "%02X%02X%02X", d.mtp[0x17], d.mtp[0x18], d.mtp[0x19]);
  size_t used = 0;
  for (size_t i = 0x1A; i <= 0x27 && used + 3 <= lutSize; i++) {
    used += static_cast<size_t>(snprintf(lut + used, lutSize - used, "%02X", d.mtp[i]));
  }
}
}  // namespace

const char* controllerName() {
  switch (BoardConfig::ACTIVE.displayController) {
    case DisplayController::SSD1677:
      return "SSD1677";
    case DisplayController::UC8253:
      return "UC8253";
    case DisplayController::ED2208:
      return "ED2208";
    case DisplayController::LgfxEpd:
      return "LGFX parallel";
    case DisplayController::IT8951:
      return "IT8951";
    case DisplayController::UC8279:
      return "UC8279";
    case DisplayController::UC8179:
      return "UC8179";
    case DisplayController::UC8279C:
      return "UC8279C";
  }
  return "unknown";
}

void logPanel() {
  const auto& d = freeink::getXteinkDisplayProbeDiag();
  char id[8];
  char lut[32];
  formatPanelId(d, id, sizeof(id), lut, sizeof(lut));
  LOG_DBG("PANEL", "controller=%s variant=0x%02X via %s promoted=%d", controllerName(),
          BoardConfig::ACTIVE.displayControllerVariant, detectMethod(d), d.promoted ? 1 : 0);
  if (d.valid) {
    LOG_DBG("PANEL", "VER=%02X %02X %02X %02X %02X FLG=%02X verdict=%u busyTimeout=%d productId=%s lutVer=%s", d.ver[0],
            d.ver[1], d.ver[2], d.ver[3], d.ver[4], d.flg, d.verdict, d.busyTimedOut ? 1 : 0, id, lut);
  }
  if (d.mtpValid) {
    char hex[sizeof(d.mtp) * 3 + 1];
    size_t used = 0;
    for (size_t i = 0; i < sizeof(d.mtp); i++) {
      used += static_cast<size_t>(snprintf(hex + used, sizeof(hex) - used, "%02X ", d.mtp[i]));
    }
    LOG_DBG("PANEL", "MTP[0x000..0x02F]: %s", hex);
  }
}

size_t formatLogHeader(char* buf, const size_t size) {
  const auto& d = freeink::getXteinkDisplayProbeDiag();
  char id[8];
  char lut[32];
  formatPanelId(d, id, sizeof(id), lut, sizeof(lut));
  const uint64_t mac = ESP.getEfuseMac();
  const unsigned long upS = millis() / 1000;
  char sec[96];
  DeviceSecurity::format(sec, sizeof(sec));
  const int n = snprintf(buf, size,
                         "=== CrossDink log dump: device=%s serial=%04X%08lX fw=%s sha=%s%s env=%s build=%s\n"
                         "=== panel=%s variant=0x%02X via %s VER=%02X %02X %02X %02X %02X productId=%s lutVer=%s\n"
                         "=== sec: %s\n"
                         "=== uptime=%lu:%02lu:%02lu heap free=%u min=%u maxAlloc=%u psram free=%u ===\n",
                         BoardConfig::ACTIVE.name, static_cast<unsigned>(mac >> 32),
                         static_cast<unsigned long>(mac & 0xFFFFFFFFu), CROSSDINK_VERSION, BuildInfo::gitSha(),
                         strcmp(BuildInfo::gitDirty(), "1") == 0 ? "*" : "", CROSSDINK_PIOENV, BuildInfo::buildNumber(),
                         controllerName(), BoardConfig::ACTIVE.displayControllerVariant, detectMethod(d), d.ver[0],
                         d.ver[1], d.ver[2], d.ver[3], d.ver[4], id, lut, sec, upS / 3600, (upS / 60) % 60, upS % 60,
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (n <= 0) return 0;
  return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

#else  // release or simulator

const char* controllerName() { return "unknown"; }
void logPanel() {}
size_t formatLogHeader(char* buf, const size_t size) {
  if (size > 0) buf[0] = '\0';
  return 0;
}

#endif

}  // namespace DeviceIdentity
