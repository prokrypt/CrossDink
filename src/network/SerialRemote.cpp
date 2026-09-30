#include "SerialRemote.h"

#if CROSSDINK_SERIAL_REMOTE

#include <Arduino.h>
#include <FreeInkDisplay.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <InputManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "OpdsServerStore.h"
#include "SettingsList.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/goodies/GoodiesActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace SerialRemote {
namespace {

constexpr uint32_t DEFAULT_TAP_MS = 80;
constexpr uint32_t DEFAULT_TOUCH_MS = 60;
constexpr uint32_t DEFAULT_SWIPE_MS = 300;
constexpr uint32_t TOUCH_RELEASE_MS = 100;  // no-contact samples before real touch resumes
constexpr uint32_t SWIPE_END_HOLD_MS = 40;
constexpr uint32_t DEFAULT_WAIT_MS = 10000;
// Idle must hold this long so an injected release has reached the activity
// loop (and its requestUpdate) before WAITIDLE answers.
constexpr uint32_t IDLE_SETTLE_MS = 150;

// Read by the input sampling task through the SDK hooks.
std::atomic<uint8_t> buttonMask{0};
std::atomic<bool> touchActive{false};
std::atomic<bool> touchDown{false};
std::atomic<float> touchX{0.0f};
std::atomic<float> touchY{0.0f};

// Main task only.
bool hooksInstalled = false;
uint32_t keyReleaseAt = 0;
uint8_t keyReleaseBit = 0;

enum class TouchPhase : uint8_t { Idle, Hold, Swipe, Release };
TouchPhase touchPhase = TouchPhase::Idle;
uint32_t touchPhaseEnd = 0;
uint32_t swipeStart = 0;
uint32_t swipeMs = 0;
float swipeX0 = 0, swipeY0 = 0, swipeX1 = 0, swipeY1 = 0;

std::string typeQueue;  // debug builds only; bounded by the serial line length
size_t typePos = 0;

bool waitPending = false;
uint32_t waitStart = 0;
uint32_t waitTimeout = 0;
uint32_t idleSince = 0;

// Wi-Fi remote hand-off. state: 0 idle, 3 being filled, 1 queued by the server task, 2 main
// task ran it and waits for the reply line (WAITIDLE answers later).
// ponytail: one command at a time; a serial reply landing while state is 2
// goes to HTTP instead. Fine for a single tester.
constexpr const char* TOKEN_PATH = "/debug/remote-token";
constexpr size_t TOKEN_MAX = 64;
std::atomic<uint8_t> httpState{0};
SemaphoreHandle_t httpDone = nullptr;
char httpLine[260];
char httpToken[TOKEN_MAX + 2];
char httpReply[256];
int httpStatus = 0;

// Wi-Fi screenshot: PBM (P4) image, inverted from the framebuffer's 1 = white.
// Static in PSRAM (debug x4-pro only) so a grab never needs a 48 KB heap block.
constexpr size_t SNAP_MAX = 48000 + 32;  // largest current panel + PBM header
EXT_RAM_NOINIT_ATTR uint8_t snap[SNAP_MAX];
size_t snapLen = 0;

// GOTO launches this long after its reply, so the HTTP response is out before a
// screen change (or a network reboot) can stop the server that carries it.
constexpr uint32_t GOTO_DELAY_MS = 500;
int gotoPending = -1;  // index into kGoto
uint32_t gotoAt = 0;

void finishHttp(const int status) {
  httpStatus = status;
  httpState.store(0, std::memory_order_release);
  xSemaphoreGive(httpDone);
}

uint8_t buttonHook() { return buttonMask.load(std::memory_order_relaxed); }

bool touchHook(float& nx, float& ny, bool& down) {
  if (!touchActive.load(std::memory_order_acquire)) return false;
  nx = touchX.load(std::memory_order_relaxed);
  ny = touchY.load(std::memory_order_relaxed);
  down = touchDown.load(std::memory_order_relaxed);
  return true;
}

void installHooks() {
  if (hooksInstalled) return;
  InputManager::setButtonHook(&buttonHook);
  InputManager::setTouchHook(&touchHook);
  hooksInstalled = true;
}

void reply(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(buf, sizeof(buf) - 1, fmt, args);
  va_end(args);
  const size_t len = n < 0 ? 0 : std::min(static_cast<size_t>(n), sizeof(buf) - 2);
  if (httpState.load(std::memory_order_acquire) == 2) {
    memcpy(httpReply, buf, len);
    httpReply[len] = '\0';
    return finishHttp(strncmp(buf, "OK:", 3) == 0 ? 200 : 400);
  }
  buf[len] = '\n';
  // The host waits on this line; a plain print drops it while logs from other
  // tasks hold the 1 ms-timeout TX path.
  if (!logSerialWriteAll(buf, len + 1)) LOG_ERR("SER", "Reply dropped: %.*s", static_cast<int>(len), buf);
}

int buttonBit(const char* name) {
  static const struct {
    const char* name;
    uint8_t index;
  } kButtons[] = {{"back", HalGPIO::BTN_BACK},   {"confirm", HalGPIO::BTN_CONFIRM}, {"left", HalGPIO::BTN_LEFT},
                  {"right", HalGPIO::BTN_RIGHT}, {"up", HalGPIO::BTN_UP},           {"down", HalGPIO::BTN_DOWN},
                  {"power", HalGPIO::BTN_POWER}};
  for (const auto& b : kButtons) {
    if (strcasecmp(name, b.name) == 0) return b.index;
  }
  return -1;
}

// Framebuffer (panel-native) pixels to the touch frame's 0..1 range.
void toNormalized(const float x, const float y, float& nx, float& ny) {
  const float w = display.getDisplayWidth() > 0 ? display.getDisplayWidth() : 1;
  const float h = display.getDisplayHeight() > 0 ? display.getDisplayHeight() : 1;
  nx = x / w;
  ny = y / h;
}

void setTouch(const float nx, const float ny, const bool down) {
  touchX.store(nx, std::memory_order_relaxed);
  touchY.store(ny, std::memory_order_relaxed);
  touchDown.store(down, std::memory_order_relaxed);
  touchActive.store(true, std::memory_order_release);
}

bool inputBusy() { return keyReleaseAt != 0 || touchPhase != TouchPhase::Idle || typePos < typeQueue.size(); }

// Length of the UTF-8 sequence starting at `c`.
size_t utf8Len(const unsigned char c) {
  if (c < 0x80) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

void cmdKey(char* args) {
  char* save = nullptr;
  const char* name = strtok_r(args, " ", &save);
  const char* action = strtok_r(nullptr, " ", &save);
  const char* msArg = strtok_r(nullptr, " ", &save);
  const int bit = name ? buttonBit(name) : -1;
  if (bit < 0) return reply("ERR:KEY:unknown_button");
  const uint8_t mask = static_cast<uint8_t>(1U << bit);
  if (action == nullptr || strcasecmp(action, "tap") == 0) {
    if (keyReleaseAt != 0) return reply("ERR:KEY:busy");
    const uint32_t ms = msArg ? static_cast<uint32_t>(strtoul(msArg, nullptr, 10)) : DEFAULT_TAP_MS;
    buttonMask.fetch_or(mask);
    keyReleaseBit = mask;
    keyReleaseAt = millis() + (ms ? ms : 1);
  } else if (strcasecmp(action, "down") == 0) {
    buttonMask.fetch_or(mask);
  } else if (strcasecmp(action, "up") == 0) {
    buttonMask.fetch_and(static_cast<uint8_t>(~mask));
  } else {
    return reply("ERR:KEY:bad_action");
  }
  reply("OK:KEY");
}

void cmdTouch(char* args) {
  float x = 0, y = 0;
  unsigned ms = DEFAULT_TOUCH_MS;
  if (sscanf(args, "%f %f %u", &x, &y, &ms) < 2) return reply("ERR:TOUCH:args");
  if (touchPhase != TouchPhase::Idle) return reply("ERR:TOUCH:busy");
  float nx = 0, ny = 0;
  toNormalized(x, y, nx, ny);
  setTouch(nx, ny, true);
  touchPhase = TouchPhase::Hold;
  touchPhaseEnd = millis() + (ms ? ms : 1);
  reply("OK:TOUCH");
}

void cmdSwipe(char* args) {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  unsigned ms = DEFAULT_SWIPE_MS;
  if (sscanf(args, "%f %f %f %f %u", &x0, &y0, &x1, &y1, &ms) < 4) return reply("ERR:SWIPE:args");
  if (touchPhase != TouchPhase::Idle) return reply("ERR:SWIPE:busy");
  toNormalized(x0, y0, swipeX0, swipeY0);
  toNormalized(x1, y1, swipeX1, swipeY1);
  swipeStart = millis();
  swipeMs = ms ? ms : 1;
  setTouch(swipeX0, swipeY0, true);
  touchPhase = TouchPhase::Swipe;
  reply("OK:SWIPE");
}

void cmdType(const char* text) {
  if (text == nullptr || *text == '\0') return reply("ERR:TYPE:empty");
  if (typePos < typeQueue.size()) return reply("ERR:TYPE:busy");
  typeQueue.assign(text);
  typePos = 0;
  reply("OK:TYPE %u", static_cast<unsigned>(typeQueue.size()));
}

void cmdHeap() {
  reply("OK:HEAP internal_free=%u internal_min=%u internal_largest=%u psram_free=%u psram_largest=%u",
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
}

void cmdStatus() {
  const float chipC = temperatureRead();
  reply(
      "OK:STATUS {\"activity\":\"%s\",\"uptimeMs\":%lu,\"renderIdle\":%s,\"fb\":{\"w\":%u,\"h\":%u,\"bytes\":%u},"
      "\"internalFree\":%u,\"internalMin\":%u,\"psramFree\":%u,\"chipC\":%.1f}",
      activityManager.currentActivityName(), static_cast<unsigned long>(millis()),
      activityManager.isRenderIdle() ? "true" : "false", static_cast<unsigned>(display.getDisplayWidth()),
      static_cast<unsigned>(display.getDisplayHeight()), static_cast<unsigned>(display.getBufferSize()),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), std::isnan(chipC) ? -999.0f : chipC);
}

void cmdSet(char* args) {
  char* save = nullptr;
  const char* key = strtok_r(args, " ", &save);
  const char* valueArg = strtok_r(nullptr, " ", &save);
  if (key == nullptr || valueArg == nullptr) return reply("ERR:SET:args");
  const long value = strtol(valueArg, nullptr, 0);
  for (const auto& s : getBaseSettingsList()) {
    if (s.key == nullptr || strcmp(s.key, key) != 0) continue;
    switch (s.type) {
      case SettingType::TOGGLE:
        if (s.valuePtr == nullptr) return reply("ERR:SET:unsupported");
        SETTINGS.*(s.valuePtr) = value ? 1 : 0;
        break;
      case SettingType::ENUM: {
        if (s.valuePtr == nullptr) return reply("ERR:SET:unsupported");
        bool valid = false;
        if (!s.enumRawValues.empty()) {
          for (const uint8_t raw : s.enumRawValues) valid = valid || raw == value;
        } else {
          const size_t count = s.enumStringValues.empty() ? s.enumValues.size() : s.enumStringValues.size();
          valid = value >= 0 && static_cast<size_t>(value) < count;
        }
        if (!valid) return reply("ERR:SET:range");
        SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(value);
        break;
      }
      case SettingType::VALUE:
        if (value < s.valueRange.min || value > s.valueRange.max) return reply("ERR:SET:range");
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(value);
        } else if (s.value16Ptr) {
          SETTINGS.*(s.value16Ptr) = static_cast<uint16_t>(value);
        } else {
          return reply("ERR:SET:unsupported");
        }
        break;
      default:
        return reply("ERR:SET:unsupported");
    }
    if (!SETTINGS.saveToFile()) return reply("ERR:SET:save");
    return reply("OK:SET %s %ld", key, value);
  }
  reply("ERR:SET:unknown_key");
}

// Overrides the keyboard refresh experiment in RAM (applied at the next
// keyboard open, kept until "off" or reboot). No SD write.
void cmdKbdExp(char* args) {
  while (*args == ' ') args++;
  if (strcasecmp(args, "off") == 0) {
    KeyboardEntryActivity::clearExperimentOverride();
    return reply("OK:KBDEXP off");
  }
  int flags = 0;
  int frames = 6;
  int pll = 0;
  if (sscanf(args, "%i %i %i", &flags, &frames, &pll) < 1 || flags < 0 || flags > 255 || frames < 0 || frames > 63 ||
      pll < 0 || pll > 255) {
    return reply("ERR:KBDEXP:args");
  }
  KeyboardEntryActivity::setExperimentOverride(static_cast<uint8_t>(flags), static_cast<uint8_t>(frames),
                                               static_cast<uint8_t>(pll));
  reply("OK:KBDEXP %d %d %d", flags, frames, pll);
}

void cmdRefresh(const char* mode) {
  HalDisplay::RefreshMode m = HalDisplay::FAST_REFRESH;
  if (mode != nullptr && strcasecmp(mode, "half") == 0) {
    m = HalDisplay::HALF_REFRESH;
  } else if (mode != nullptr && strcasecmp(mode, "full") == 0) {
    m = HalDisplay::FULL_REFRESH;
  } else if (mode != nullptr && *mode != '\0' && strcasecmp(mode, "fast") != 0) {
    return reply("ERR:REFRESH:mode");
  }
  RenderLock lock;
  display.displayBuffer(m);
  reply("OK:REFRESH");
}

// Main task: copies the framebuffer under the render lock into `snap` as a PBM.
void takeSnapshot() {
  RenderLock lock;
  const uint32_t w = display.getDisplayWidth();
  const uint32_t h = display.getDisplayHeight();
  const uint32_t bytes = display.getBufferSize();
  const int headerLen = snprintf(reinterpret_cast<char*>(snap), 32, "P4\n%lu %lu\n", static_cast<unsigned long>(w),
                                 static_cast<unsigned long>(h));
  if (headerLen <= 0 || headerLen + bytes > SNAP_MAX) {
    snapLen = 0;
    return reply("ERR:SCREENSHOT:too_big");
  }
  const uint8_t* fb = display.getFrameBuffer();
  for (uint32_t i = 0; i < bytes; i++) snap[headerLen + i] = static_cast<uint8_t>(~fb[i]);
  snapLen = headerLen + bytes;
  reply("OK:SCREENSHOT %lu %lu %lu", static_cast<unsigned long>(w), static_cast<unsigned long>(h),
        static_cast<unsigned long>(bytes));
}

// Constant time over the whole buffer; empty or missing token file = disabled.
bool tokenMatches(const char* given) {
  static char stored[TOKEN_MAX + 2];
  memset(stored, 0, sizeof(stored));
  if (!Storage.exists(TOKEN_PATH)) return false;
  size_t n = Storage.readFileToBuffer(TOKEN_PATH, stored, sizeof(stored));
  while (n > 0 && isspace(static_cast<unsigned char>(stored[n - 1]))) stored[--n] = '\0';
  if (n == 0 || n > TOKEN_MAX) return false;
  uint8_t diff = strlen(given) != n;
  for (size_t i = 0; i < sizeof(stored); i++) diff |= static_cast<uint8_t>(stored[i] ^ given[i]);
  return diff == 0;
}

// Main task: runs a command queued by runFromOtherTask().
void pollHttp() {
  if (httpState.load(std::memory_order_acquire) != 1) return;
  if (!tokenMatches(httpToken)) {
    LOG_ERR("SER", "Wi-Fi remote: bad token");
    strcpy(httpReply, "ERR:token");
    return finishHttp(403);
  }
  httpState.store(2, std::memory_order_release);
  if (strcmp(httpLine, "CMD:SCREENSHOT") == 0) return takeSnapshot();
  if (!handleLine(httpLine)) {
    strcpy(httpReply, "ERR:unknown_cmd");
    finishHttp(404);
  }
}

bool hasResumeBook() { return !APP_STATE.openEpubPath.empty() && Storage.exists(APP_STATE.openEpubPath.c_str()); }

// ReaderActivity hands over to the format's reader at once.
const char* readerName() {
  const std::string& path = APP_STATE.openEpubPath;
  if (FsHelpers::hasEpubExtension(path)) return "EpubReader";
  if (FsHelpers::hasXtcExtension(path)) return "XtcReader";
  if (FsHelpers::hasTxtExtension(path)) return "TxtReader";
  return "Reader";
}

// Top-level screens for CMD:GOTO. `activity` is the name ACTIVITY reports once it is up.
// Screens that drop Wi-Fi (Wi-Fi networks, transfer, Calibre, OPDS, nearby) end the remote.
struct GotoTarget {
  const char* name;
  const char* activity;
  bool (*available)();  // nullptr = always
  void (*launch)();
};
const GotoTarget kGoto[] = {
    {"home", "Home", nullptr, [] { activityManager.goHome(); }},
    {"files", "FileBrowser", nullptr, [] { activityManager.goToFileBrowser(); }},
    {"library", "Library", nullptr, [] { activityManager.goToLibrary(); }},
    {"reader", "Reader", &hasResumeBook, [] { activityManager.goToReader(APP_STATE.openEpubPath); }},
    {"settings", "Settings", nullptr, [] { activityManager.goToSettings(); }},
    {"wifi", "WifiSelection", nullptr,
     [] {
       activityManager.replaceActivity(std::make_unique<WifiSelectionActivity>(renderer, mappedInputManager, false));
     }},
#if CROSSDINK_GOODIES
    {"goodies", "Goodies", nullptr,
     [] { activityManager.replaceActivity(std::make_unique<GoodiesActivity>(renderer, mappedInputManager)); }},
#endif
    {"transfer", "CrossPointWebServer", nullptr, [] { activityManager.goToFileTransfer(); }},
    {"transfer-wifi", "CrossPointWebServer", nullptr, [] { activityManager.goToJoinNetworkFileTransfer(); }},
    {"transfer-hotspot", "CrossPointWebServer", nullptr, [] { activityManager.goToHotspotFileTransfer(); }},
    {"calibre", "CrossPointWebServer", nullptr, [] { activityManager.goToCalibreWireless(); }},
    {"opds", "OpdsBookBrowser", [] { return OPDS_STORE.hasServers(); }, [] { activityManager.goToBrowser(); }},
    {"nearby", "NearbyBookTransfer", nullptr, [] { activityManager.goToNearbyBookReceive(); }},
    {"nearby-stats", "NearbyStatsSync", [] { return SETTINGS.shouldTrackReadingStats(); },
     [] { activityManager.goToNearbyStatsSync(); }},
#if CROSSDINK_APP_CAP_USB_DRIVE
    {"usb", "UsbDrive", nullptr, [] { activityManager.goToUsbDrive(); }},
#endif
};

void cmdGoto(const char* name) {
  if (strcasecmp(name, "list") == 0) {
    char names[200];
    size_t n = 0;
    names[0] = '\0';
    for (const auto& t : kGoto) {
      if (n < sizeof(names)) n += snprintf(names + n, sizeof(names) - n, " %s", t.name);
    }
    return reply("OK:GOTO list%s", names);
  }
  if (gotoPending >= 0) return reply("ERR:GOTO:busy");
  for (size_t i = 0; i < sizeof(kGoto) / sizeof(kGoto[0]); i++) {
    const auto& t = kGoto[i];
    if (strcasecmp(name, t.name) != 0) continue;
    if (t.available && !t.available()) return reply("ERR:GOTO:unavailable");
    gotoPending = static_cast<int>(i);
    gotoAt = millis() + GOTO_DELAY_MS;
    const char* activity = t.activity;
    if (t.available == &hasResumeBook) activity = readerName();
    if (strcmp(t.name, "opds") == 0 && OPDS_STORE.getCount() > 1) activity = "OpdsServerList";  // server picker
    return reply("OK:GOTO %s", activity);
  }
  reply("ERR:GOTO:unknown_screen");
}

void cmdWaitIdle(const char* arg) {
  waitPending = true;
  waitStart = millis();
  waitTimeout = (arg && *arg) ? static_cast<uint32_t>(strtoul(arg, nullptr, 10)) : DEFAULT_WAIT_MS;
  idleSince = 0;
}

}  // namespace

bool handleLine(const char* line) {
  if (strncmp(line, "CMD:", 4) != 0) return false;
  char buf[256];
  strncpy(buf, line + 4, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  char* args = strchr(buf, ' ');
  if (args) {
    *args++ = '\0';
  } else {
    args = buf + strlen(buf);
  }
  const char* verb = buf;
  LOG_DBG("SER", "cmd %s", verb);  // PSRAM log shows which commands arrived
  installHooks();

  if (strcmp(verb, "PING") == 0) {
    reply("OK:PING");
  } else if (strcmp(verb, "KEY") == 0) {
    cmdKey(args);
  } else if (strcmp(verb, "TOUCH") == 0) {
    cmdTouch(args);
  } else if (strcmp(verb, "SWIPE") == 0) {
    cmdSwipe(args);
  } else if (strcmp(verb, "TYPE") == 0) {
    cmdType(args);
  } else if (strcmp(verb, "STATUS") == 0) {
    cmdStatus();
  } else if (strcmp(verb, "ACTIVITY") == 0) {
    reply("OK:ACTIVITY %s", activityManager.currentActivityName());
  } else if (strcmp(verb, "HEAP") == 0) {
    cmdHeap();
  } else if (strcmp(verb, "FBINFO") == 0) {
    reply("OK:FBINFO %u %u %u", static_cast<unsigned>(display.getDisplayWidth()),
          static_cast<unsigned>(display.getDisplayHeight()), static_cast<unsigned>(display.getBufferSize()));
  } else if (strcmp(verb, "SET") == 0) {
    cmdSet(args);
  } else if (strcmp(verb, "KBDEXP") == 0) {
    cmdKbdExp(args);
  } else if (strcmp(verb, "REFRESH") == 0) {
    cmdRefresh(args);
  } else if (strcmp(verb, "OTPREAD") == 0) {
    // PROBE (read-only): UC8179 OTP voltage/VCOM headers; run on a B/W screen (Home).
    freeink::requestUc8179OtpReadNext();
    {
      RenderLock lock;
      display.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    reply("OK:OTPREAD %s", freeink::uc8179OtpReport());
  } else if (strcmp(verb, "HOME") == 0) {
    activityManager.goHome();
    reply("OK:HOME");
  } else if (strcmp(verb, "OPEN") == 0) {
    if (*args == '\0' || !Storage.exists(args)) {
      reply("ERR:OPEN:not_found");
    } else {
      activityManager.goToReader(std::string(args));
      reply("OK:OPEN");
    }
  } else if (strcmp(verb, "SLEEP") == 0) {
    reply("OK:SLEEP");
    activityManager.goToSleep();
  } else if (strcmp(verb, "REBOOT") == 0) {
    reply("OK:REBOOT");
    logSerial.flush();
    delay(50);
    ESP.restart();  // intentional: remote-control reboot for test runs
  } else if (strcmp(verb, "WAITIDLE") == 0) {
    cmdWaitIdle(args);
  } else if (strcmp(verb, "GOTO") == 0) {
    cmdGoto(args);
  } else {
    return false;  // SCREENSHOT, PSRAMLOG and future commands stay with the caller
  }
  return true;
}

void poll() {
  pollHttp();
  const uint32_t now = millis();

  if (gotoPending >= 0 && static_cast<int32_t>(now - gotoAt) >= 0) {
    const int i = gotoPending;
    gotoPending = -1;
    LOG_INF("SER", "GOTO %s", kGoto[i].name);
    kGoto[i].launch();
  }

  if (keyReleaseAt != 0 && static_cast<int32_t>(now - keyReleaseAt) >= 0) {
    buttonMask.fetch_and(static_cast<uint8_t>(~keyReleaseBit));
    keyReleaseAt = 0;
  }

  switch (touchPhase) {
    case TouchPhase::Idle:
      break;
    case TouchPhase::Hold:
      if (static_cast<int32_t>(now - touchPhaseEnd) >= 0) {
        touchDown.store(false, std::memory_order_relaxed);
        touchPhase = TouchPhase::Release;
        touchPhaseEnd = now + TOUCH_RELEASE_MS;
      }
      break;
    case TouchPhase::Swipe: {
      const uint32_t elapsed = now - swipeStart;
      if (elapsed >= swipeMs) {
        setTouch(swipeX1, swipeY1, true);
        touchPhase = TouchPhase::Hold;
        touchPhaseEnd = now + SWIPE_END_HOLD_MS;
      } else {
        const float t = static_cast<float>(elapsed) / static_cast<float>(swipeMs);
        setTouch(swipeX0 + (swipeX1 - swipeX0) * t, swipeY0 + (swipeY1 - swipeY0) * t, true);
      }
      break;
    }
    case TouchPhase::Release:
      if (static_cast<int32_t>(now - touchPhaseEnd) >= 0) {
        touchActive.store(false, std::memory_order_release);
        touchPhase = TouchPhase::Idle;
      }
      break;
  }

  // One character per frame, so each keystroke renders like a real key press.
  if (typePos < typeQueue.size() && activityManager.isRenderIdle()) {
    char ch[5] = {};
    const size_t n = std::min(utf8Len(static_cast<unsigned char>(typeQueue[typePos])), typeQueue.size() - typePos);
    memcpy(ch, typeQueue.data() + typePos, n);
    typePos += n;
    if (!activityManager.injectText(ch)) {
      reply("ERR:TYPE:no_text_input");
      typeQueue.clear();
      typePos = 0;
    } else if (typePos >= typeQueue.size()) {
      typeQueue.clear();
      typePos = 0;
    }
  }

  if (waitPending) {
    const bool busy = inputBusy() || !activityManager.isRenderIdle();
    if (busy) {
      idleSince = 0;
    } else if (idleSince == 0) {
      idleSince = now;
    } else if (now - idleSince >= IDLE_SETTLE_MS) {
      waitPending = false;
      reply("OK:WAITIDLE %lu", static_cast<unsigned long>(now - waitStart));
    }
    if (waitPending && now - waitStart >= waitTimeout) {
      waitPending = false;
      reply("ERR:WAITIDLE:timeout");
    }
  }
}

int runFromOtherTask(const char* token, const char* cmd, char* out, const size_t outLen, const uint32_t timeoutMs) {
  static bool init = false;  // server task only
  if (!init) {
    httpDone = xSemaphoreCreateBinary();
    init = true;
  }
  uint8_t idle = 0;
  if (!httpState.compare_exchange_strong(idle, 3, std::memory_order_acq_rel)) {
    snprintf(out, outLen, "ERR:busy");
    return 503;
  }
  xSemaphoreTake(httpDone, 0);  // drop a give left by a timed-out request
  // Zero-padded so tokenMatches() can compare the full buffer.
  memset(httpToken, 0, sizeof(httpToken));
  strncpy(httpToken, token, sizeof(httpToken) - 1);
  snprintf(httpLine, sizeof(httpLine), "CMD:%s", cmd);
  httpState.store(1, std::memory_order_release);
  if (xSemaphoreTake(httpDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    uint8_t queued = 1;
    // Still queued: withdraw it. Otherwise the main task owns it and frees it.
    httpState.compare_exchange_strong(queued, 0, std::memory_order_acq_rel);
    snprintf(out, outLen, "ERR:timeout");
    return 503;
  }
  snprintf(out, outLen, "%s", httpReply);
  return httpStatus;
}

const uint8_t* screenshot(size_t& len) {
  len = snapLen;
  return snap;
}

}  // namespace SerialRemote

#endif  // CROSSDINK_SERIAL_REMOTE
