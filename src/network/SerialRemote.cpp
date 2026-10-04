#include "SerialRemote.h"

#include <strings.h>

bool SerialRemote::isTokenPath(const char* path, const bool orFolder) {
  // FAT matches names case-insensitively, drops trailing dots and spaces, and
  // also opens the 8.3 alias (REMOTE~1), so compare what the card would open.
  // A dots-only segment could step back into /debug: refuse those anywhere.
  int seg = 0;
  bool match = true;
  for (const char* p = path; *p;) {
    while (*p == '/') p++;
    const char* s = p;
    while (*p && *p != '/') p++;
    size_t n = p - s;
    if (n == 0) break;
    while (n && (s[n - 1] == '.' || s[n - 1] == ' ')) n--;
    if (n == 0) return true;
    if (seg == 0) {
      match = match && n == 5 && strncasecmp(s, "debug", 5) == 0;
    } else if (seg == 1) {
      match = match &&
              ((n == 12 && strncasecmp(s, "remote-token", 12) == 0) || (n > 7 && strncasecmp(s, "remote~", 7) == 0));
    }
    seg++;
  }
  return match && (seg == 2 || (orFolder && seg == 1));
}

#if CROSSDINK_SERIAL_REMOTE

#include <Arduino.h>
#include <BatteryMonitor.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <InputManager.h>
#include <Knobs.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_reg.h>
#include <soc/io_mux_reg.h>

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
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/goodies/GoodiesActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "activities/util/RemoteImageActivity.h"
#include "platform/InputTask.h"
#include "platform/PinMon.h"
#include "util/UrlUtils.h"

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
std::atomic<uint8_t> httpState{0};
SemaphoreHandle_t httpDone = nullptr;
char httpLine[260];
char httpToken[TOKEN_MAX + 2];
char httpReply[256];
int httpStatus = 0;
uint32_t httpIp = 0;

// Wi-Fi screenshot: PGM (P5, 4 levels) while a gray pass is on the panel, else
// PBM (P4) inverted from the framebuffer's 1 = white. Static in PSRAM (debug
// x4-pro only) so a grab never needs a large heap block.
constexpr size_t SNAP_MAX = 800 * 480 + 32;  // largest current panel, 1 byte/pixel + header
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

// GAUGEINT [seconds]: which GPIO, if any, carries the CW2017 INT_N line.
// INT_N is an open-drain ~1 ms low pulse per alert event (datasheet
// "Interrupt"), so falling edges are counted by interrupt on the unused pads:
// first a 5 s baseline, then `seconds` with every gauge alert forced on
// (temperature above TEMP_MAX and below TEMP_MIN, SoC alert on any 1% change),
// clearing the flags each second so every re-detection pulses again. Runs on
// its own task: the reply comes at once and the result is a [SER] GAUGEINT log
// line. Registers and pads are restored afterwards.
namespace gaugeint {
constexpr uint8_t PINS[] = {15, 16, 17, 45, 46, 47, 48};  // unused on the X4 Pro
constexpr size_t N = sizeof(PINS);
std::atomic<uint32_t> edges[N];
std::atomic<bool> running{false};
uint32_t seconds = 30;

void IRAM_ATTR onEdge(void* arg) { edges[reinterpret_cast<uintptr_t>(arg)].fetch_add(1, std::memory_order_relaxed); }

void run(void*) {
  static const BatteryMonitor gauge;
  constexpr uint8_t REG_TEMP = 0x06, REG_INT_CONF = 0x0A, REG_SOC_ALERT = 0x0B, REG_TEMP_MAX = 0x0C,
                    REG_TEMP_MIN = 0x0D;
  constexpr uint8_t EN_ALL = 0x70, SOC_ANY_CHANGE = 0xFF;  // 0x0B: UPDATE_FLAG kept, threshold 0x7F
  constexpr uint32_t BASE_MS = 5000;
  uint8_t intConf = 0, socAlert = 0, tempMax = 0, tempMin = 0, temp = 0;
  if (!gauge.readGaugeReg(REG_INT_CONF, intConf) || !gauge.readGaugeReg(REG_SOC_ALERT, socAlert) ||
      !gauge.readGaugeReg(REG_TEMP_MAX, tempMax) || !gauge.readGaugeReg(REG_TEMP_MIN, tempMin) ||
      !gauge.readGaugeReg(REG_TEMP, temp) || temp < 8 || temp > 247) {
    LOG_ERR("SER", "GAUGEINT: gauge read failed");
    running = false;
    vTaskDelete(nullptr);
    return;
  }
  uint32_t mux[N];
  gpio_install_isr_service(0);  // usually already installed by InputWake (then INVALID_STATE)
  for (size_t i = 0; i < N; ++i) {
    const auto pin = static_cast<gpio_num_t>(PINS[i]);
    mux[i] = REG_READ(GPIO_PIN_MUX_REG[PINS[i]]);
    edges[i] = 0;
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_pullup_en(pin);
    gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(pin, onEdge, reinterpret_cast<void*>(i));
    gpio_intr_enable(pin);
  }
  vTaskDelay(pdMS_TO_TICKS(BASE_MS));
  uint32_t base[N];
  for (size_t i = 0; i < N; ++i) base[i] = edges[i].exchange(0);

  bool armed = gauge.writeGaugeReg(REG_TEMP_MAX, static_cast<uint8_t>(temp - 8)) &&
               gauge.writeGaugeReg(REG_TEMP_MIN, static_cast<uint8_t>(temp + 8)) &&
               gauge.writeGaugeReg(REG_SOC_ALERT, SOC_ANY_CHANGE) &&
               gauge.writeGaugeReg(REG_INT_CONF, static_cast<uint8_t>((intConf & 0x80) | EN_ALL));
  uint8_t seen = 0;
  uint32_t reads = 0;
  for (uint32_t s = 0; armed && s < seconds; ++s) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint8_t v = 0;
    if (gauge.readGaugeReg(REG_INT_CONF, v)) {
      ++reads;
      seen |= v & 0x0F;
      if (v & 0x0F)
        gauge.writeGaugeReg(REG_INT_CONF, static_cast<uint8_t>(v & 0xF0));  // clear: the next detection pulses again
    }
  }

  const bool restored = gauge.writeGaugeReg(REG_TEMP_MAX, tempMax) && gauge.writeGaugeReg(REG_TEMP_MIN, tempMin) &&
                        gauge.writeGaugeReg(REG_SOC_ALERT, socAlert) &&
                        gauge.writeGaugeReg(REG_INT_CONF, static_cast<uint8_t>(intConf & 0xF0));
  char pins[128] = "";
  size_t n = 0;
  for (size_t i = 0; i < N; ++i) {
    const auto pin = static_cast<gpio_num_t>(PINS[i]);
    gpio_intr_disable(pin);
    gpio_isr_handler_remove(pin);
    gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
    REG_WRITE(GPIO_PIN_MUX_REG[PINS[i]], mux[i]);
    n += snprintf(pins + n, sizeof(pins) - n, " %u:%lu/%lu", PINS[i], static_cast<unsigned long>(base[i]),
                  static_cast<unsigned long>(edges[i].load()));
  }
  // flags: REG_INT_CONF[3:0] seen while armed (bit2 SOC, bit1 TMX, bit0 TMN).
  LOG_INF("SER", "GAUGEINT done %lus armed=%d flags=0x%X reads=%lu restored=%d edges(base %lus/armed):%s",
          static_cast<unsigned long>(seconds), armed, seen, static_cast<unsigned long>(reads), restored,
          static_cast<unsigned long>(BASE_MS / 1000), pins);
  running = false;
  vTaskDelete(nullptr);
}
}  // namespace gaugeint

void cmdGaugeInt(const char* args) {
  const unsigned long s = *args ? strtoul(args, nullptr, 10) : 30;
  if (s < 5 || s > 600) {
    reply("ERR:GAUGEINT:seconds_5_to_600");
    return;
  }
  if (PinMon::enabled()) {
    reply("ERR:GAUGEINT:pinmon_on");  // both use the same pins: PINMON off first
    return;
  }
  if (gaugeint::running.exchange(true)) {
    reply("ERR:GAUGEINT:busy");
    return;
  }
  gaugeint::seconds = s;
  if (xTaskCreate(gaugeint::run, "gaugeint", 4096, nullptr, 1, nullptr) != pdPASS) {
    gaugeint::running = false;
    reply("ERR:GAUGEINT:task");
    return;
  }
  reply("OK:GAUGEINT started %lus (+5s baseline); result in the log as [SER] GAUGEINT done", s);
}

// PINMON [on|off|status]: the passive unused-pin monitor (src/platform/PinMon.h).
void cmdPinMon(const char* args) {
  if (strcmp(args, "on") == 0) {
    if (gaugeint::running) {
      reply("ERR:PINMON:gaugeint_running");
      return;
    }
    PinMon::setEnabled(true);
  } else if (strcmp(args, "off") == 0) {
    PinMon::setEnabled(false);
  } else if (*args && strcmp(args, "status") != 0) {
    reply("ERR:PINMON:on_off_status");
    return;
  }
  char s[200];
  PinMon::status(s, sizeof(s));
  reply("OK:PINMON %s", s);
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

// A setting's value as SET takes it (enums raw). Strings are quoted, and only
// these are readable; any other string (credentials) reads "****". False:
// nothing to read (actions, headers).
bool settingValue(const SettingInfo& s, char* out, const size_t len) {
  switch (s.type) {
    case SettingType::TOGGLE:
    case SettingType::ENUM:
    case SettingType::VALUE:
      // The light as it is now (the panel and gestures set it apart from SETTINGS).
      if (s.valuePtr == &CrossPointSettings::frontlightBrightness)
        return snprintf(out, len, "%u", Frontlight.brightness()) > 0;
      if (s.valuePtr == &CrossPointSettings::frontlightWarmth) return snprintf(out, len, "%u", Frontlight.warmth()) > 0;
      if (s.valuePtr == &CrossPointSettings::frontlightOn)
        return snprintf(out, len, "%u", Frontlight.isOn() ? 1 : 0) > 0;
      if (s.valuePtr) return snprintf(out, len, "%u", SETTINGS.*(s.valuePtr)) > 0;
      if (s.value16Ptr) return snprintf(out, len, "%u", SETTINGS.*(s.value16Ptr)) > 0;
      if (s.valueGetter) return snprintf(out, len, "%u", s.valueGetter()) > 0;
      return false;
    case SettingType::STRING: {
      static const char* const READABLE[] = {"deviceName", "opdsDownloadFolder", "nearbyReceiveFolder", "koServerUrl"};
      bool readable = false;
      for (const char* k : READABLE) readable = readable || strcmp(s.key, k) == 0;
      if (!readable) return snprintf(out, len, "\"****\"") > 0;
      std::string v = s.stringGetter       ? s.stringGetter()
                      : s.stringMaxLen > 0 ? std::string(reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset)
                                           : std::string();
      return snprintf(out, len, "\"%s\"", UrlUtils::maskUserInfo(v).c_str()) > 0;
    }
    default:
      return false;
  }
}

// SET <key> <value> | SET get <key> | SET list [from]: settings by web API
// key, as cmdKnob pages its list.
void cmdSet(char* args) {
  char* save = nullptr;
  const char* key = strtok_r(args, " ", &save);
  const char* valueArg = strtok_r(nullptr, " ", &save);
  if (key != nullptr && strcasecmp(key, "list") == 0) {
    const auto& list = getBaseSettingsList();
    char out[232];
    char value[96];
    int n = 0;
    size_t i = valueArg ? std::max(0, atoi(valueArg)) : 0;
    for (; i < list.size(); ++i) {
      if (list[i].key == nullptr || !settingValue(list[i], value, sizeof(value))) continue;
      const int w = snprintf(out + n, sizeof(out) - n, " %s=%s", list[i].key, value);
      if (w < 0 || n + w >= static_cast<int>(sizeof(out)) - 12) break;
      n += w;
    }
    out[n] = '\0';
    return i < list.size() ? reply("OK:SET%s next=%u", out, static_cast<unsigned>(i)) : reply("OK:SET%s", out);
  }
  if (key != nullptr && strcasecmp(key, "get") == 0) {
    char value[160];
    for (const auto& s : getBaseSettingsList()) {
      if (s.key == nullptr || valueArg == nullptr || strcmp(s.key, valueArg) != 0) continue;
      if (!settingValue(s, value, sizeof(value))) return reply("ERR:SET:unsupported");
      if (s.type == SettingType::VALUE) {
        return reply("OK:SET %s %s min=%d max=%d step=%d", s.key, value, s.valueRange.min, s.valueRange.max,
                     s.valueRange.step);
      }
      if (s.type == SettingType::ENUM && !s.enumRawValues.empty()) {
        char raws[64];
        int n = 0;
        for (const uint8_t raw : s.enumRawValues) {
          const int w = snprintf(raws + n, sizeof(raws) - n, "%s%u", n ? "," : "", raw);
          if (w < 0 || n + w >= static_cast<int>(sizeof(raws))) break;
          n += w;
        }
        return reply("OK:SET %s %s values=%s", s.key, value, n ? raws : "-");
      }
      if (s.type == SettingType::ENUM) {
        const size_t count = s.enumStringValues.empty() ? s.enumValues.size() : s.enumStringValues.size();
        return reply("OK:SET %s %s values=0-%u", s.key, value, static_cast<unsigned>(count ? count - 1 : 0));
      }
      return reply("OK:SET %s %s", s.key, value);
    }
    return reply("ERR:SET:unknown_key");
  }
  if (key == nullptr || valueArg == nullptr) return reply("ERR:SET:args");
  const long value = strtol(valueArg, nullptr, 0);
  // An open book holds its effective reader values in SETTINGS and restores the
  // globals it saw at open on exit. Edit and save the globals with the book's
  // values set aside, as the Frontlight panel does; it reapplies them after.
  // Then repaint, so the screen shows the change without a button press.
  struct BookAside {
    bool on = activityManager.beginGlobalSettingsEdit();
    ~BookAside() {
      if (on) activityManager.endGlobalSettingsEdit();
      activityManager.requestUpdate();
    }
  } bookAside;
  for (const auto& s : getBaseSettingsList()) {
    if (s.key == nullptr || strcmp(s.key, key) != 0) continue;
    switch (s.type) {
      case SettingType::TOGGLE:
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = value ? 1 : 0;
        } else if (s.valueSetter) {
          s.valueSetter(value ? 1 : 0);  // saves its own store (KOReader)
        } else {
          return reply("ERR:SET:unsupported");
        }
        break;
      case SettingType::ENUM: {
        if (s.valuePtr == nullptr && !s.valueSetter) return reply("ERR:SET:unsupported");
        bool valid = false;
        if (!s.enumRawValues.empty()) {
          for (const uint8_t raw : s.enumRawValues) valid = valid || raw == value;
        } else {
          const size_t count = s.enumStringValues.empty() ? s.enumValues.size() : s.enumStringValues.size();
          valid = value >= 0 && static_cast<size_t>(value) < count;
        }
        if (!valid) return reply("ERR:SET:range");
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(value);
        } else {
          s.valueSetter(static_cast<uint8_t>(value));  // saves its own store (KOReader)
        }
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
    if (s.valuePtr) applySettingChange(s.valuePtr);  // what the Settings menu applies after a change
    if (!SETTINGS.saveToFile()) return reply("ERR:SET:save");
    return reply("OK:SET %s %ld", key, value);
  }
  reply("ERR:SET:unknown_key");
}

#if CROSSDINK_GOODIES
// Goodies > Knobs (lib/Knobs/Knobs.def). One reply line each, so "list" pages:
// "id=value ..." from index `from`, with "next=N" while more remain. "changed"
// pages the same way but only knobs off their default (the on-device *), as
// "id=value/default".
void cmdKnob(char* args) {
  char* save = nullptr;
  const char* sub = strtok_r(args, " ", &save);
  const char* id = strtok_r(nullptr, " ", &save);
  const char* valueArg = strtok_r(nullptr, " ", &save);
  const bool changedOnly = sub != nullptr && strcasecmp(sub, "changed") == 0;
  if (sub == nullptr || strcasecmp(sub, "list") == 0 || changedOnly) {
    char out[232];
    int n = 0;
    int i = id ? std::max(0, atoi(id)) : 0;
    for (; i < knobs::COUNT; ++i) {
      const knobs::Info& k = knobs::INFO[i];
      const long v = static_cast<long>(knobs::get(i));
      if (changedOnly && v == k.def) continue;
      const int w = changedOnly ? snprintf(out + n, sizeof(out) - n, " %s=%ld/%ld", k.id, v, static_cast<long>(k.def))
                                : snprintf(out + n, sizeof(out) - n, " %s=%ld", k.id, v);
      if (w < 0 || n + w >= static_cast<int>(sizeof(out)) - 12) break;
      n += w;
    }
    out[n] = '\0';
    return i < knobs::COUNT ? reply("OK:KNOB%s next=%d", out, i) : reply("OK:KNOB%s", out);
  }
  if (strcasecmp(sub, "reset") == 0 && id == nullptr) {
    knobs::resetAll();
    return reply("OK:KNOB reset");
  }
  const int i = id ? knobs::find(id) : -1;
  if (i < 0) return reply("ERR:KNOB:unknown_id");
  const knobs::Info& k = knobs::INFO[i];
  if (strcasecmp(sub, "get") == 0) {
    return reply("OK:KNOB %s %ld def=%ld min=%ld max=%ld step=%ld %s", k.id, static_cast<long>(knobs::get(i)),
                 static_cast<long>(k.def), static_cast<long>(k.min), static_cast<long>(k.max),
                 static_cast<long>(k.step), k.unit);
  }
  if (strcasecmp(sub, "reset") == 0) return reply("OK:KNOB %s %ld", k.id, static_cast<long>(knobs::set(i, k.def)));
  if (strcasecmp(sub, "set") == 0 && valueArg != nullptr) {
    return reply("OK:KNOB %s %ld", k.id, static_cast<long>(knobs::set(i, strtol(valueArg, nullptr, 0))));
  }
  reply("ERR:KNOB:args");
}
#endif

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
  // pll: a knobs::PLL_BYTES index (0 default, 1 = 40 Hz, 2 = 50 Hz), never a raw 0x30 byte.
  if (sscanf(args, "%i %i %i", &flags, &frames, &pll) < 1 || flags < 0 || flags > 255 || frames < 0 || frames > 63 ||
      pll < 0 || pll >= knobs::PLL_CHOICES) {
    LOG_ERR("SR", "KBDEXP: bad args (flags 0-255, frames 0-63, pll 0-%d)", knobs::PLL_CHOICES - 1);
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

// Main task: shows the frame POST /api/image just received (the server task waits
// meanwhile); a picture already up takes it in place.
void cmdImage() {
  if (!RemoteImageActivity::upload) return reply("ERR:IMAGE:no_data");
  RemoteImageActivity::incoming = std::move(RemoteImageActivity::upload);
  if (strcmp(activityManager.currentActivityName(), "RemoteImage") != 0) {
    activityManager.pushActivity(std::make_unique<RemoteImageActivity>(renderer, mappedInputManager));
  }
  reply("OK:IMAGE");
}

// Main task: copies the screen under the render lock into `snap` as a PGM
// (last gray pass still shown) or a PBM.
void takeSnapshot() {
  RenderLock lock;
  const uint32_t w = display.getDisplayWidth();
  const uint32_t h = display.getDisplayHeight();
  char* header = reinterpret_cast<char*>(snap);
  uint32_t bytes = w * h;
  int headerLen =
      snprintf(header, 32, "P5\n%lu %lu\n255\n", static_cast<unsigned long>(w), static_cast<unsigned long>(h));
  if (headerLen > 0 && headerLen + bytes <= SNAP_MAX && display.grayShotReady()) {
    uint8_t* out = snap + headerLen;
    for (uint32_t y = 0; y < h; y++) {
      for (uint32_t x = 0; x < w; x++) *out++ = display.grayShotLevel(x, y) * 85;  // 0/85/170/255
    }
  } else {
    bytes = display.getBufferSize();
    headerLen = snprintf(header, 32, "P4\n%lu %lu\n", static_cast<unsigned long>(w), static_cast<unsigned long>(h));
    if (headerLen <= 0 || headerLen + bytes > SNAP_MAX) {
      snapLen = 0;
      return reply("ERR:SCREENSHOT:too_big");
    }
    const uint8_t* fb = display.getFrameBuffer();
    for (uint32_t i = 0; i < bytes; i++) snap[headerLen + i] = static_cast<uint8_t>(~fb[i]);
  }
  snapLen = headerLen + bytes;
  reply("OK:SCREENSHOT %lu %lu %lu", static_cast<unsigned long>(w), static_cast<unsigned long>(h),
        static_cast<unsigned long>(bytes));
}

// Constant time over the whole buffer; empty or missing token file = disabled.
bool tokenMatches(const char* given) {
  static char stored[TOKEN_BUF];
  const size_t n = readToken(stored);
  if (n == 0) return false;
  uint8_t diff = strlen(given) != n;
  for (size_t i = 0; i < sizeof(stored); i++) diff |= static_cast<uint8_t>(stored[i] ^ given[i]);
  return diff == 0;
}

// Bad tokens per client IP, so a short PIN can't be brute-forced: 5 misses lock
// that IP for 60 s, doubling per lock up to 64 min; a good token clears it.
// Per IP, so a stranger's misses never lock out another client. Main task only.
// ponytail: 8 slots; with every slot locked, new IPs are refused too (fails
// closed, a many-IP flood blocks everyone until the locks run out). RAM only.
struct Strikes {
  uint32_t ip;
  uint8_t misses;
  uint8_t locks;
  uint32_t lockedUntil;  // millis(); 0 = not locked
};
Strikes strikes[8];

bool lockActive(const Strikes& s, const uint32_t now) {
  return s.lockedUntil != 0 && static_cast<int32_t>(s.lockedUntil - now) > 0;
}

// The IP's slot, else the free or least-struck unlocked slot; null when all are locked.
Strikes* strikesFor(const uint32_t ip, const uint32_t now) {
  Strikes* pick = nullptr;
  for (Strikes& s : strikes) {
    if (s.ip == ip) return &s;
    if (!lockActive(s, now) && (!pick || s.misses + s.locks < pick->misses + pick->locks)) pick = &s;
  }
  if (pick) *pick = {ip, 0, 0, 0};
  return pick;
}

// Main task: runs a command queued by runFromOtherTask().
void pollHttp() {
  if (httpState.load(std::memory_order_acquire) != 1) return;
  const uint32_t now = millis();
  Strikes* s = strikesFor(httpIp, now);
  if (!s || lockActive(*s, now)) {
    strcpy(httpReply, "ERR:locked");
    return finishHttp(429);
  }
  if (!tokenMatches(httpToken)) {
    LOG_WRN("SER", "Wi-Fi remote: bad token");
    strcpy(httpReply, "ERR:token");
    if (++s->misses >= 5) {
      s->misses = 0;
      s->lockedUntil = (now + (60000u << std::min<uint8_t>(s->locks, 6))) | 1;
      if (s->locks < 6) s->locks++;
      LOG_ERR("SER", "Wi-Fi remote: client locked out");
    }
    return finishHttp(403);
  }
  *s = {httpIp, 0, 0, 0};
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

size_t readToken(char (&out)[TOKEN_BUF]) {
  memset(out, 0, TOKEN_BUF);
  if (!Storage.exists(TOKEN_PATH)) return 0;
  size_t n = Storage.readFileToBuffer(TOKEN_PATH, out, TOKEN_BUF);
  while (n > 0 && isspace(static_cast<unsigned char>(out[n - 1]))) out[--n] = '\0';
  if (n > TOKEN_MAX) n = 0;
  if (n == 0) memset(out, 0, TOKEN_BUF);
  return n;
}

size_t newPin(char (&out)[TOKEN_BUF]) {
  uint32_t r;
  do {
    r = esp_random();
  } while (r >= 4294000000u);  // whole millions only, so every PIN is equally likely
  memset(out, 0, TOKEN_BUF);
  snprintf(out, TOKEN_BUF, "%06lu", static_cast<unsigned long>(r % 1000000));
  if (!Storage.ensureDirectoryExists("/debug") || !Storage.writeFile(TOKEN_PATH, String(out) + "\n")) {
    LOG_ERR("SER", "Wi-Fi remote: could not write a new PIN");
    memset(out, 0, TOKEN_BUF);
    return 0;
  }
  for (Strikes& s : strikes) s = {};
  LOG_INF("SER", "Wi-Fi remote: new PIN written");
  return 6;
}

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
  } else if (strcmp(verb, "GAUGEINT") == 0) {
    cmdGaugeInt(args);
  } else if (strcmp(verb, "PINMON") == 0) {
    cmdPinMon(args);
  } else if (strcmp(verb, "FBINFO") == 0) {
    reply("OK:FBINFO %u %u %u", static_cast<unsigned>(display.getDisplayWidth()),
          static_cast<unsigned>(display.getDisplayHeight()), static_cast<unsigned>(display.getBufferSize()));
  } else if (strcmp(verb, "SET") == 0) {
    cmdSet(args);
#if CROSSDINK_GOODIES
  } else if (strcmp(verb, "KNOB") == 0) {
    cmdKnob(args);
#endif
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
    // /api/ota lands here too: save what the open screens save on exit first.
    activityManager.exitAllActivities();
    silentRestart();  // intentional: remote-control reboot for test runs, no logo
  } else if (strcmp(verb, "WAITIDLE") == 0) {
    cmdWaitIdle(args);
  } else if (strcmp(verb, "GOTO") == 0) {
    cmdGoto(args);
  } else if (strcmp(verb, "IMAGE") == 0) {
    cmdImage();
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

int runFromOtherTask(const char* token, const char* cmd, const uint32_t clientIp, char* out, const size_t outLen,
                     const uint32_t timeoutMs) {
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
  httpIp = clientIp;
  httpState.store(1, std::memory_order_release);
  InputTask::wakeLoop();  // the loop may be in a long idle wait
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
