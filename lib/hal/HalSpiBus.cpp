#include "HalSpiBus.h"

#include <Logging.h>

HalSpiBus::HalSpiBus() {
  mutex = xSemaphoreCreateRecursiveMutex();
  if (mutex == nullptr) {
    LOG_ERR("SPI", "Failed to create SPI bus mutex - bus is unusable");
  }
}

HalSpiBus& HalSpiBus::getInstance() {
  static HalSpiBus spiBus;
  return spiBus;
}

HalSpiBus::Lock::Lock() {
  auto& bus = HalSpiBus::getInstance();
  if (bus.mutex == nullptr) {
    LOG_ERR("SPI", "SPI bus mutex not initialized, skipping lock");
    return;
  }
  const BaseType_t takeResult = xSemaphoreTakeRecursive(bus.mutex, portMAX_DELAY);
  if (takeResult != pdTRUE) {
    LOG_ERR("SPI", "Failed to acquire SPI bus mutex");
    return;
  }
  acquired = true;
  bus.depth++;
}

HalSpiBus::Lock::~Lock() {
  if (!acquired) return;
  auto& bus = HalSpiBus::getInstance();
  bus.depth--;
  xSemaphoreGiveRecursive(bus.mutex);
}

bool HalSpiBus::releaseForIdle() {
  if (mutex == nullptr || xSemaphoreGetMutexHolder(mutex) != xTaskGetCurrentTaskHandle() || depth != 1) {
    return false;
  }
  depth = 0;
  xSemaphoreGiveRecursive(mutex);
  return true;
}

void HalSpiBus::reacquireAfterIdle() {
  xSemaphoreTakeRecursive(mutex, portMAX_DELAY);
  depth = 1;
}
