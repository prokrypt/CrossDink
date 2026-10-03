#include "PinMon.h"

#if CROSSDINK_SERIAL_REMOTE && defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)

#include <Arduino.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <esp_pm.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_struct.h>

#include <atomic>
#include <cstdio>

namespace {
constexpr uint8_t PINS[] = {15, 16, 17, 45, 46, 47, 48};  // unused on the X4 Pro
constexpr size_t N = sizeof(PINS);
static_assert(N == PinMon::PIN_COUNT);
constexpr uint32_t CHATTER_PER_MIN = 20;
constexpr uint32_t SUMMARY_MS = 60000;

struct Edge {
  int64_t us;
  uint8_t level;
  bool wake;
};
Edge edges[N];                             // ISR writes slot i, then sets bit i of fired
std::atomic<uint32_t> fired{0};            // slots holding an unread edge
uint8_t armedFor[N];                       // level each pin waits for (ISR reads; DRAM)
volatile uint32_t wokeLo = 0, wokeHi = 0;  // our pins pending at light-sleep exit
uint32_t maskLo = 0, maskHi = 0;
TaskHandle_t task = nullptr;
std::atomic<bool> want{false};
bool on = false;
uint32_t chatter = 0;  // bit i: released until reboot
uint32_t perMin[N], total[N], wakes[N], mux[N];

// arg = slot | pin << 8, so the ISR reads nothing from flash.
void IRAM_ATTR onLine(void* arg) {
  const auto v = reinterpret_cast<uintptr_t>(arg);
  const uint32_t i = v & 0xFF, pin = v >> 8;
  gpio_ll_intr_disable(&GPIO, pin);  // level interrupt: the task re-arms it
  volatile uint32_t& woke = pin < 32 ? wokeLo : wokeHi;
  const uint32_t bit = 1u << (pin & 31);
  edges[i] = {esp_timer_get_time(), armedFor[i], (woke & bit) != 0};
  woke = woke & ~bit;
  fired.fetch_or(1u << i, std::memory_order_release);
  BaseType_t hp = pdFALSE;
  vTaskNotifyGiveFromISR(task, &hp);
  if (hp == pdTRUE) portYIELD_FROM_ISR();
}

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
// Runs before pending ISRs after every light sleep: a pin of ours already
// pending then is what (or part of what) ended the sleep.
esp_err_t IRAM_ATTR onSleepExit(int64_t, void*) {
  wokeLo = wokeLo | (GPIO.status & maskLo);
  wokeHi = wokeHi | (GPIO.status1.val & maskHi);
  return ESP_OK;
}
#endif

void arm(const size_t i, const uint8_t level) {
  const auto pin = static_cast<gpio_num_t>(PINS[i]);
  armedFor[i] = level;
  if (PINS[i] < 32) {
    wokeLo = wokeLo & ~(1u << PINS[i]);
  } else {
    wokeHi = wokeHi & ~(1u << (PINS[i] - 32));
  }
  gpio_wakeup_enable(pin, level ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
  gpio_intr_enable(pin);
}

void claim(const size_t i) {
  const auto pin = static_cast<gpio_num_t>(PINS[i]);
  mux[i] = REG_READ(GPIO_PIN_MUX_REG[PINS[i]]);
  // 45/46 are strapping pins: keep their reset-default pull-down.
  const bool strap = PINS[i] == 45 || PINS[i] == 46;
  gpio_config_t c = {};
  c.pin_bit_mask = 1ULL << PINS[i];
  c.mode = GPIO_MODE_INPUT;
  c.pull_up_en = strap ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE;
  c.pull_down_en = strap ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE;
  c.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&c);
  gpio_isr_handler_add(pin, onLine, reinterpret_cast<void*>(i | PINS[i] << 8));
  arm(i, !gpio_get_level(pin));
}

void release(const size_t i) {
  const auto pin = static_cast<gpio_num_t>(PINS[i]);
  gpio_intr_disable(pin);
  gpio_wakeup_disable(pin);
  gpio_isr_handler_remove(pin);
  gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
  gpio_set_pull_mode(pin, GPIO_FLOATING);
  REG_WRITE(GPIO_PIN_MUX_REG[PINS[i]], mux[i]);
}

void run(void*) {
  uint32_t windowStart = millis();
  for (;;) {
    const uint32_t elapsed = millis() - windowStart;
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(elapsed >= SUMMARY_MS ? 0 : SUMMARY_MS - elapsed));
    if (want != on) {
      on = want;
      fired = 0;
      for (size_t i = 0; i < N; ++i) {
        if (!(chatter & 1u << i)) on ? claim(i) : release(i);
      }
      LOG_INF("PIN", "PINMON %s", on ? "on" : "off");
    }
    const uint32_t f = fired.exchange(0, std::memory_order_acquire);
    for (size_t i = 0; on && i < N; ++i) {
      if (!(f & 1u << i)) continue;
      const Edge e = edges[i];
      ++perMin[i];
      ++total[i];
      wakes[i] += e.wake;
      LOG_INF("PIN", "PINMON %s%u %s t=%lu", e.wake ? "wake pin " : "", PINS[i], e.level ? "rise" : "fall",
              static_cast<unsigned long>(e.us / 1000));
      if (perMin[i] > CHATTER_PER_MIN) {
        chatter |= 1u << i;
        release(i);
        LOG_INF("PIN", "PINMON %u chatter, disabled", PINS[i]);
        continue;
      }
      arm(i, !e.level);  // already back? fires at once and logs the return edge
    }
    if (millis() - windowStart < SUMMARY_MS) continue;
    windowStart = millis();
    if (!on) continue;
    char line[160];
    size_t n = 0;
    for (size_t i = 0; i < N; ++i) {
      n += snprintf(line + n, sizeof(line) - n, " %u:%lu", PINS[i], static_cast<unsigned long>(perMin[i]));
      perMin[i] = 0;
    }
    LOG_INF("PIN", "PINMON 1m changes%s chatter=0x%02lX", line, static_cast<unsigned long>(chatter));
  }
}
}  // namespace

void PinMon::begin() {
  if (task) return;
  for (const uint8_t p : PINS) (p < 32 ? maskLo : maskHi) |= 1u << (p & 31);
  gpio_install_isr_service(0);  // usually already installed by InputWake (then INVALID_STATE)
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
  esp_pm_sleep_cbs_register_config_t cbs = {};
  cbs.exit_cb = onSleepExit;
  if (esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK) LOG_ERR("PIN", "PINMON: sleep exit hook failed, no wake tags");
#endif
  if (xTaskCreate(run, "pinmon", 4096, nullptr, 2, &task) != pdPASS) {
    task = nullptr;
    LOG_ERR("PIN", "PINMON: task create failed");
  }
}

void PinMon::setEnabled(const bool enable) {
  want = enable;
  if (task) xTaskNotifyGive(task);
}

bool PinMon::enabled() { return want; }

PinMon::PinStat PinMon::stat(const size_t i) {
  return {PINS[i], gpio_get_level(static_cast<gpio_num_t>(PINS[i])), total[i], wakes[i], (chatter & 1u << i) != 0};
}

void PinMon::status(char* out, const size_t len) {
  size_t n = snprintf(out, len, "%s", want ? "on" : "off");
  for (size_t i = 0; i < N && n < len; ++i) {
    const PinStat p = stat(i);
    n += snprintf(out + n, len - n, " %u:%d/%lu/%lu", p.gpio, p.level, static_cast<unsigned long>(p.changes),
                  static_cast<unsigned long>(p.wakes));
  }
  if (n < len) snprintf(out + n, len - n, " chatter=0x%02lX", static_cast<unsigned long>(chatter));
}

#endif
