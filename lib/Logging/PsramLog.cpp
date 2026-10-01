#include "PsramLog.h"

#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)

#include <esp_attr.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "PsramRing.h"

#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error "CROSSDINK_PSRAM_LOG needs CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y"
#endif

namespace {
// Power of two so the ring position is a mask of the running byte count.
constexpr uint32_t kRingBytes = 512 * 1024;
constexpr uint32_t kMagicA = 0x50534C47;  // "PSLG"
constexpr uint32_t kMagicB = 0xC0DE0513;

using Ring = PsramRing<kRingBytes>;  // aux counts restarts
EXT_RAM_NOINIT_ATTR Ring ring;

// Internal RAM, zeroed every boot.
portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
bool initDone = false;
// waitForAppend(): append() gives it while a reader waits.
SemaphoreHandle_t appendSignal = nullptr;
std::atomic<bool> readerWaiting{false};

void appendRaw(const char* text, const uint32_t len) {
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t startHead = ring.head;
  ring.write(text, len);
  portEXIT_CRITICAL_SAFE(&ringMux);
  ring.writeBack(startHead, len);
}

// Header as found at boot, reported once in the first line this boot writes.
uint32_t foundMagicA = 0, foundMagicB = 0, foundSize = 0, foundHead = 0;
bool headerKept = false;
bool bootLinePending = false;

// Belt and braces for the per-append write-back: flush the header once more
// on the way down through esp_restart().
void flushHeaderOnRestart() { ring.writeBackHeader(); }

// PSRAM is mapped by Arduino's psramInit() inside initArduino(), after global
// constructors run. Touching the ring before that reads an unmapped address
// and would stamp a fresh header over the surviving one, so every entry point
// waits until PSRAM is up; lines logged earlier are not kept.
bool ensureInit() {
  if (initDone) return true;
  if (!esp_psram_is_initialized()) return false;
  bool didInit = false;
  portENTER_CRITICAL_SAFE(&ringMux);
  if (!initDone) {
    foundMagicA = ring.magicA;
    foundMagicB = ring.magicB;
    foundSize = ring.size;
    foundHead = ring.head;
    headerKept = ring.adopt(kMagicA, kMagicB);
    if (headerKept) ring.aux++;
    bootLinePending = true;
    initDone = true;
    didInit = true;
  }
  portEXIT_CRITICAL_SAFE(&ringMux);
  if (didInit) {
    ring.writeBackHeader();
    esp_register_shutdown_handler(flushHeaderOnRestart);
  }
  return true;
}

void appendBootLineIfPending() {
  if (!bootLinePending) return;
  bootLinePending = false;
  char line[200];
  int len = snprintf(line, sizeof(line), "\n=== boot: PSRAM log %s, restart #%lu, reset reason %d, ring@%p",
                     headerKept ? "kept" : "reset", static_cast<unsigned long>(ring.aux),
                     static_cast<int>(esp_reset_reason()), static_cast<void*>(&ring));
  // The found header is uninitialized PSRAM after a cold boot; show it only
  // when it was valid and kept.
  if (len > 0 && headerKept && len < static_cast<int>(sizeof(line))) {
    len += snprintf(line + len, sizeof(line) - len, ", found magic %08lx/%08lx size %lu head %lu",
                    static_cast<unsigned long>(foundMagicA), static_cast<unsigned long>(foundMagicB),
                    static_cast<unsigned long>(foundSize), static_cast<unsigned long>(foundHead));
  }
  if (len > 0 && len < static_cast<int>(sizeof(line))) {
    len += snprintf(line + len, sizeof(line) - len, " ===\n");
  }
  if (len > 0) appendRaw(line, static_cast<uint32_t>(std::min<int>(len, sizeof(line) - 1)));
}
}  // namespace

namespace PsramLog {

void append(const char* text, const size_t len) {
  if (len == 0 || !ensureInit()) return;
  appendBootLineIfPending();
  appendRaw(text, static_cast<uint32_t>(len));
  if (readerWaiting.load(std::memory_order_acquire)) {
    xPortInIsrContext() ? xSemaphoreGiveFromISR(appendSignal, nullptr) : xSemaphoreGive(appendSignal);
  }
}

bool waitForAppend(const uint32_t since, const uint32_t timeoutMs) {
  if (!appendSignal) appendSignal = xSemaphoreCreateBinary();  // first long-poll, server task
  if (!appendSignal) {
    vTaskDelay(pdMS_TO_TICKS(timeoutMs));
    return end() != since;
  }
  const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(timeoutMs);
  xSemaphoreTake(appendSignal, 0);  // a give left from an earlier wait
  readerWaiting.store(true, std::memory_order_release);
  while (end() == since) {
    const TickType_t left = until - xTaskGetTickCount();
    if (static_cast<int32_t>(left) <= 0 || xSemaphoreTake(appendSignal, left) != pdTRUE) break;
  }
  readerWaiting.store(false, std::memory_order_release);
  return end() != since;
}

uint32_t oldest() {
  if (!ensureInit()) return 0;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  portEXIT_CRITICAL_SAFE(&ringMux);
  return head > kRingBytes ? head - kRingBytes : 0;
}

uint32_t end() {
  if (!ensureInit()) return 0;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  portEXIT_CRITICAL_SAFE(&ringMux);
  return head;
}

size_t read(uint32_t& cursor, char* dst, const size_t maxLen) {
  if (!ensureInit()) return 0;
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  const uint32_t oldestByte = head > kRingBytes ? head - kRingBytes : 0;
  if (cursor < oldestByte || cursor > head) cursor = oldestByte;
  const uint32_t len = std::min<uint32_t>(head - cursor, static_cast<uint32_t>(maxLen));
  ring.copyOut(cursor, dst, len);
  portEXIT_CRITICAL_SAFE(&ringMux);
  cursor += len;
  return len;
}

}  // namespace PsramLog

#endif
