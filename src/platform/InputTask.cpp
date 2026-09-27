#include "InputTask.h"

#include <Arduino.h>

#include "InputWake.h"

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR) && FREEINK_MCU_S3

#include <HalGPIO.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "TaskCores.h"

namespace {
// Matches the loop's active tick and InputManager's debounce and GT911 poll
// cadence while a key or contact is down.
constexpr uint32_t ACTIVE_POLL_MS = 8;
// With every input on a wake line the idle wait only bounds a missed edge.
constexpr uint32_t IDLE_COVERED_WAIT_MS = 1000;
constexpr uint32_t IDLE_POLLED_WAIT_MS = 20;
// InputManager::update() plus a GT911/I2C read; same budget as the SDK's own
// async input task.
constexpr uint32_t STACK_BYTES = 4096;
// Above loopTask (2), so a sample is never queued behind loop work on core 0.
constexpr UBaseType_t PRIORITY = 3;

SemaphoreHandle_t loopWake = nullptr;
TaskHandle_t inputTask = nullptr;

void inputTaskMain(void*) {
  const uint32_t idleWaitMs = InputWake::coversAllInputs() ? IDLE_COVERED_WAIT_MS : IDLE_POLLED_WAIT_MS;
  for (;;) {
    const HalGPIO::SampleResult result = gpio.sampleInput();
    if (result.events) xSemaphoreGive(loopWake);
    if (result.active) {
      vTaskDelay(pdMS_TO_TICKS(ACTIVE_POLL_MS));
    } else {
      InputWake::wait(idleWaitMs);
    }
  }
}
}  // namespace

void InputTask::begin() {
  if (inputTask) return;
  loopWake = xSemaphoreCreateBinary();
  if (!loopWake || !gpio.startLatchedInput()) {
    LOG_ERR("INPUT", "Input task unavailable; sampling on the loop");
    return;
  }
  if (xTaskCreatePinnedToCore(inputTaskMain, "Input", STACK_BYTES, nullptr, PRIORITY, &inputTask, TaskCores::kWorker) !=
      pdPASS) {
    inputTask = nullptr;
    gpio.stopLatchedInput();
    LOG_ERR("INPUT", "Cannot start input task; sampling on the loop");
    return;
  }
  LOG_INF("INPUT", "Input sampled on its own task (core %d)", static_cast<int>(TaskCores::kWorker));
}

void InputTask::waitForInput(const uint32_t timeoutMs) {
  if (!inputTask) {
    InputWake::wait(timeoutMs);
    return;
  }
  xSemaphoreTake(loopWake, pdMS_TO_TICKS(timeoutMs));
}

#else

void InputTask::begin() {}

void InputTask::waitForInput(const uint32_t timeoutMs) { InputWake::wait(timeoutMs); }

#endif
