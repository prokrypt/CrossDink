#include "InputWake.h"

#include <Arduino.h>

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR)

#include <BoardConfig.h>
#include <Logging.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#include <soc/rtc_io_struct.h>
#if CROSSDINK_PERF_LOG && CONFIG_IDF_TARGET_ESP32S3
#include <esp_attr.h>
#include <soc/gpio_periph.h>
#include <soc/io_mux_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/rtc_io_reg.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>

namespace {
std::array<gpio_num_t, 10> wakePins{};  // keys, touch INT, charger STAT
size_t wakePinCount = 0;
std::array<bool, 10> armedHigh{};  // level each line was last armed for
SemaphoreHandle_t wakeSignal = nullptr;
bool allInputsCovered = false;
// Charger STAT: not an input, but a charge start or stop should end the wait
// so the loop sees USB/charging change without a poll tick.
int8_t chargePin = -1;
std::atomic<bool> chargeWoke{false};
#if CROSSDINK_PERF_LOG
// Line interrupts per kind for the [PM] wake counts (ISR writes; DRAM).
int8_t touchWakePin = -1;
std::atomic<uint32_t> buttonWakes{0};
std::atomic<uint32_t> touchWakes{0};
#endif

#if CROSSDINK_PERF_LOG && CONFIG_IDF_TARGET_ESP32S3
// STAT pad state, digital and RTC side: after a charge-wake deep sleep the RTC
// wake saw STAT at the level wait() armed against, so every light sleep was
// rejected until a power-on reset (20261001T223751Z). Logs show which RTC-domain
// bits (kept through deep sleep and SW restart) differ from a normal wake.
struct StatPad {
  uint32_t magic, gpioIn, rtcIn, iomux, pad, pin, status, hold;
};
constexpr uint32_t kStatPadMagic = 0x57A7A0D1;
RTC_NOINIT_ATTR StatPad prevBootPad;  // this boot's snapshot, printed by the next boot
bool statPadLater = true;

// S3: RTC IO number == GPIO number for GPIO0-21; pad and pin registers are 4 bytes apart.
StatPad readStatPad(const int n) {
  return {kStatPadMagic,
          static_cast<uint32_t>(gpio_get_level(static_cast<gpio_num_t>(n))),
          (REG_READ(RTC_GPIO_IN_REG) >> (RTC_GPIO_IN_NEXT_S + n)) & 1,
          REG_READ(GPIO_PIN_MUX_REG[n]),
          REG_READ(RTC_IO_TOUCH_PAD0_REG + 4 * n),
          REG_READ(RTC_GPIO_PIN0_REG + 4 * n),
          REG_READ(RTC_GPIO_STATUS_REG),
          REG_READ(RTC_CNTL_PAD_HOLD_REG)};
}

void logStatPad(const char* when, const int n, const StatPad& p) {
  const auto fieldBit = [](const uint32_t v, const int s) { return static_cast<unsigned>((v >> s) & 1); };
  LOG_INF("STATPAD",
          "%s GPIO%d gpio=%u rtc=%u | dig ie=%u pd=%u pu=%u slpsel=%u | rtc mux=%u ie=%u slpie=%u slpsel=%u rde=%u "
          "rue=%u | rtcwake en=%u type=%u st=%u hold=%u | raw iomux=%08lx pad=%08lx pin=%08lx",
          when, n, static_cast<unsigned>(p.gpioIn), static_cast<unsigned>(p.rtcIn), fieldBit(p.iomux, FUN_IE_S),
          fieldBit(p.iomux, FUN_PD_S), fieldBit(p.iomux, FUN_PU_S), fieldBit(p.iomux, SLP_SEL_S),
          fieldBit(p.pad, RTC_IO_PAD21_MUX_SEL_S), fieldBit(p.pad, RTC_IO_PAD21_FUN_IE_S),
          fieldBit(p.pad, RTC_IO_PAD21_SLP_IE_S), fieldBit(p.pad, RTC_IO_PAD21_SLP_SEL_S),
          fieldBit(p.pad, RTC_IO_PAD21_RDE_S), fieldBit(p.pad, RTC_IO_PAD21_RUE_S),
          fieldBit(p.pin, RTC_GPIO_PIN21_WAKEUP_ENABLE_S),
          static_cast<unsigned>((p.pin >> RTC_GPIO_PIN21_INT_TYPE_S) & 7),
          fieldBit(p.status, RTC_GPIO_STATUS_INT_S + n), fieldBit(p.hold, n), static_cast<unsigned long>(p.iomux),
          static_cast<unsigned long>(p.pad), static_cast<unsigned long>(p.pin));
}
#endif

// Wake lines use level interrupts, because only those can also end a light
// sleep. A level keeps firing while it holds, so each line disarms itself here
// until the next wait() re-arms it against the level it reads then.
void IRAM_ATTR onWakeLine(void* arg) {
  gpio_ll_intr_disable(&GPIO, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)));
  const auto pin = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
  if (pin == chargePin) {
    chargeWoke.store(true, std::memory_order_relaxed);
  } else {
#if CROSSDINK_PERF_LOG
    (pin == touchWakePin ? touchWakes : buttonWakes).fetch_add(1, std::memory_order_relaxed);
#endif
  }
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(wakeSignal, &higherPriorityTaskWoken);
  if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

// Returns false only when a real pin could not be armed.
bool addWakePin(const int8_t pin) {
  if (pin < 0) return true;
  if (wakePinCount >= wakePins.size()) return false;
  const auto gpioPin = static_cast<gpio_num_t>(pin);
  const auto armedEnd = wakePins.begin() + wakePinCount;
  if (std::find(wakePins.begin(), armedEnd, gpioPin) != armedEnd) return true;
  gpio_intr_disable(gpioPin);
  if (gpio_isr_handler_add(gpioPin, onWakeLine, reinterpret_cast<void*>(static_cast<uintptr_t>(pin))) != ESP_OK) {
    LOG_ERR("WAKE", "Could not attach input wake handler to GPIO%d", pin);
    return false;
  }
  wakePins[wakePinCount++] = gpioPin;
  return true;
}
}  // namespace

void InputWake::begin() {
  if (wakeSignal != nullptr) return;
#if CROSSDINK_PERF_LOG && CONFIG_IDF_TARGET_ESP32S3
  if (const int8_t s = BoardConfig::ACTIVE.batteryChargeStatus; s >= 0 && s <= 21) {
    if (prevBootPad.magic == kStatPadMagic) logStatPad("prev boot", s, prevBootPad);
    prevBootPad = readStatPad(s);
    logStatPad("boot", s, prevBootPad);
  }
#endif
  wakeSignal = xSemaphoreCreateBinary();
  if (wakeSignal == nullptr) {
    LOG_ERR("WAKE", "Could not create input wake semaphore");
    return;
  }
  // esp_restart() and panics reset only the CPUs, so the last run's GPIO
  // interrupt enables survive. A line left level-armed fires as soon as the ISR
  // service is up, before its handler is back: with no handler to disarm it, it
  // storms into an INT_WDT panic on every boot until a power-on reset (1002b:
  // GPIO21 floating high). Runs before anything else installs GPIO interrupts.
  for (int pin = 0; pin < GPIO_PIN_COUNT; ++pin) {
    if (GPIO_IS_VALID_GPIO(pin)) gpio_ll_intr_disable(&GPIO, pin);
  }
#if SOC_RTCIO_WAKE_SUPPORTED
  // RTC IO light-sleep wakes (gpio_wakeup_enable on an RTC pin) live in the RTC
  // domain, so deep sleep, esp_restart() and panics keep them. One left armed
  // by a line nothing re-arms now (Pin monitor on at sleep, an older firmware)
  // sits at its armed level and rejects every light sleep (ls=0, rjc=0x4)
  // until a power-on reset (1002b-e logs). wait() re-arms our own lines.
  uint32_t staleRtcWakes = 0;
  for (int pin = 0; pin < GPIO_PIN_COUNT; ++pin) {
    const int rtcio = rtc_io_number_get(static_cast<gpio_num_t>(pin));
    if (rtcio < 0 || !RTCIO.pin[rtcio].wakeup_enable) continue;
    staleRtcWakes |= 1u << pin;
    rtc_gpio_wakeup_disable(static_cast<gpio_num_t>(pin));
  }
  if (staleRtcWakes) {
    LOG_INF("WAKE", "Cleared RTC IO wakes left armed: GPIO mask 0x%06lx", static_cast<unsigned long>(staleRtcWakes));
  }
#endif
  // The display BUSY line may already have installed the shared ISR service.
  const esp_err_t isrErr = gpio_install_isr_service(0);
  if (isrErr != ESP_OK && isrErr != ESP_ERR_INVALID_STATE) {
    LOG_ERR("WAKE", "Could not install GPIO ISR service (%d)", static_cast<int>(isrErr));
    return;
  }

  const auto& board = BoardConfig::ACTIVE;
  bool allArmed = true;
  // ADC-ladder boards report keys through analog levels that cannot raise a
  // GPIO interrupt, so only plain digital keys take part.
  if (board.inputStyle == BoardConfig::InputStyle::DigitalButtons) {
    for (const int8_t pin : {board.input.back, board.input.confirm, board.input.left, board.input.right, board.input.up,
                             board.input.down, board.input.power}) {
      allArmed = addWakePin(pin) && allArmed;
    }
  }
  if (board.touch.controller != BoardConfig::TouchController::None) {
    allArmed = addWakePin(board.touch.irq) && allArmed;
#if CROSSDINK_PERF_LOG
    touchWakePin = board.touch.irq;
#endif
  }
  // The GT911 raises INT for every report frame while a finger or the Home key
  // is down, so one missed pulse is followed by the next. The other touch
  // controllers pulse it once or leave it unused, so they keep the poll tick.
  const bool touchCovered = board.touch.controller == BoardConfig::TouchController::None ||
                            (board.touch.controller == BoardConfig::TouchController::Gt911 && board.touch.irq >= 0);
  allInputsCovered =
      allArmed && board.inputStyle == BoardConfig::InputStyle::DigitalButtons && touchCovered && wakePinCount > 0;
  // A charger with no input power leaves STAT floating; pull it toward "not
  // charging" (as the deep-sleep ext0 wake does) so the level interrupt can't chatter.
  // STAT ends the wait while awake but is not a light-sleep wake: armed as one
  // after a deep sleep that used it for ext0, every light sleep was rejected
  // (ls=0, ~600 rejects/s; inferred from 20261001T223751Z). The 1 s idle tick
  // still sees charge changes.
  const int8_t stat = board.batteryChargeStatus;
  if (stat >= 0 && addWakePin(stat)) {
    chargePin = stat;
    const auto pin = static_cast<gpio_num_t>(stat);
    board.batteryChargeStatusActiveHigh ? gpio_pulldown_en(pin) : gpio_pullup_en(pin);
    gpio_wakeup_disable(pin);  // also clears the RTC IO wake an earlier firmware left armed
  }

  if (wakePinCount > 0 && esp_sleep_enable_gpio_wakeup() != ESP_OK) {
    LOG_ERR("WAKE", "Could not enable GPIO wake from light sleep");
    allInputsCovered = false;
  }
  LOG_INF("WAKE", "Input wake armed on %u line(s)", static_cast<unsigned>(wakePinCount));
}

void InputWake::wait(const uint32_t timeoutMs) {
  if (wakeSignal == nullptr || wakePinCount == 0) {
    delay(timeoutMs);
    return;
  }
  for (size_t i = 0; i < wakePinCount; ++i) {
    const gpio_num_t pin = wakePins[i];
    // Trigger on the opposite of the level present now, so a press, a release
    // and a touch INT pulse of either polarity all end the wait.
    const gpio_int_type_t level = gpio_get_level(pin) ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL;
    armedHigh[i] = level == GPIO_INTR_HIGH_LEVEL;
    if (static_cast<int>(pin) == chargePin) {
      gpio_set_intr_type(pin, level);
    } else {
      gpio_wakeup_enable(pin, level);
    }
    gpio_intr_enable(pin);
  }
#if CROSSDINK_PERF_LOG && CONFIG_IDF_TARGET_ESP32S3
  // Once, after the battery monitor has configured STAT.
  if (statPadLater && chargePin >= 0 && chargePin <= 21 && millis() >= 5000) {
    statPadLater = false;
    logStatPad("5s", chargePin, readStatPad(chargePin));
  }
#endif
  xSemaphoreTake(wakeSignal, pdMS_TO_TICKS(timeoutMs));
}

void InputWake::wake() {
  if (wakeSignal) xSemaphoreGive(wakeSignal);
}

bool InputWake::coversAllInputs() { return allInputsCovered; }

bool InputWake::takeChargeWake() { return chargeWoke.exchange(false, std::memory_order_relaxed); }

void InputWake::describePins(char* out, const uint32_t size) {
  size_t used = 0;
  if (size > 0) out[0] = '\0';
  for (size_t i = 0; i < wakePinCount && used < size; ++i) {
    const int pin = wakePins[i];
    const int n = snprintf(out + used, size - used, "%s%d %s%c/%d", i ? " " : "", pin, pin == chargePin ? "int:" : "",
                           armedHigh[i] ? 'H' : 'L', gpio_get_level(wakePins[i]));
    if (n <= 0) break;
    used += static_cast<size_t>(n);
  }
}

void InputWake::takeWakeCounts(uint32_t& buttons, uint32_t& touch) {
#if CROSSDINK_PERF_LOG
  buttons = buttonWakes.exchange(0, std::memory_order_relaxed);
  touch = touchWakes.exchange(0, std::memory_order_relaxed);
#else
  buttons = 0;
  touch = 0;
#endif
}

#else

void InputWake::begin() {}

void InputWake::wait(const uint32_t timeoutMs) { delay(timeoutMs); }

void InputWake::wake() {}

bool InputWake::coversAllInputs() { return false; }

bool InputWake::takeChargeWake() { return false; }

void InputWake::describePins(char* out, const uint32_t size) {
  if (size > 0) out[0] = '\0';
}

void InputWake::takeWakeCounts(uint32_t& buttons, uint32_t& touch) {
  buttons = 0;
  touch = 0;
}

#endif
