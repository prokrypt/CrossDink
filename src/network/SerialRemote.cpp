#include "SerialRemote.h"

#if CROSSDINK_SERIAL_REMOTE

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <InputManager.h>
#include <Logging.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "CrossPointSettings.h"
#include "SettingsList.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/util/KeyboardEntryActivity.h"

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
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  logSerial.printf("%s\n", buf);
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
  } else {
    return false;  // SCREENSHOT, PSRAMLOG and future commands stay with the caller
  }
  return true;
}

void poll() {
  const uint32_t now = millis();

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

}  // namespace SerialRemote

#endif  // CROSSDINK_SERIAL_REMOTE
