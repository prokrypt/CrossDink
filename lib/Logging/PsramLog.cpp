#include "PsramLog.h"

#if CROSSINK_PSRAM_LOG && !defined(SIMULATOR)

#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error "CROSSINK_PSRAM_LOG needs CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y"
#endif
#if CONFIG_SPIRAM_MEMTEST
#error "CROSSINK_PSRAM_LOG needs CONFIG_SPIRAM_MEMTEST=n (the boot memtest overwrites PSRAM)"
#endif

namespace {
// Power of two so the ring position is a mask of the running byte count.
constexpr uint32_t kRingBytes = 512 * 1024;
constexpr uint32_t kMagicA = 0x50534C47;  // "PSLG"
constexpr uint32_t kMagicB = 0xC0DE0513;

// Lives in .ext_ram_noinit: neither the heap nor the startup code touches it,
// so a software restart finds it as it was. After power loss it is random,
// which the two magic words and the size field reject.
struct Ring {
  uint32_t magicA;
  uint32_t size;
  uint32_t head;  // Total bytes ever written; position = head % kRingBytes
  uint32_t restarts;
  uint32_t magicB;
  char data[kRingBytes];
};
EXT_RAM_NOINIT_ATTR Ring ring;

// Internal RAM, zeroed every boot.
portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;
bool initDone = false;
bool restartMarkerPending = false;

// PSRAM sits behind a write-back cache. Push the new bytes out right away so
// a panic or restart that skips the cache flush still leaves them in PSRAM.
void writeBack(const void* addr, const size_t len) {
  esp_cache_msync(const_cast<void*>(addr), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

// Caller holds ringMux. Keeps only the tail of text longer than the ring.
void writeLocked(const char* text, uint32_t len) {
  if (len > kRingBytes) {
    text += len - kRingBytes;
    len = kRingBytes;
  }
  const uint32_t pos = ring.head % kRingBytes;
  const uint32_t first = std::min(len, kRingBytes - pos);
  memcpy(ring.data + pos, text, first);
  memcpy(ring.data, text + first, len - first);
  ring.head += len;
}

void writeBackRange(const uint32_t startHead, const uint32_t len) {
  const uint32_t pos = startHead % kRingBytes;
  const uint32_t first = std::min(len, kRingBytes - pos);
  writeBack(ring.data + pos, first);
  if (len > first) writeBack(ring.data, len - first);
  writeBack(&ring, offsetof(Ring, data));
}

void appendRaw(const char* text, const uint32_t len) {
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t startHead = ring.head;
  writeLocked(text, len);
  portEXIT_CRITICAL_SAFE(&ringMux);
  writeBackRange(startHead, std::min(len, kRingBytes));
}

void ensureInit() {
  if (initDone) return;
  portENTER_CRITICAL_SAFE(&ringMux);
  if (!initDone) {
    if (ring.magicA == kMagicA && ring.magicB == kMagicB && ring.size == kRingBytes) {
      ring.restarts++;
      restartMarkerPending = true;
    } else {
      ring.magicA = kMagicA;
      ring.size = kRingBytes;
      ring.head = 0;
      ring.restarts = 0;
      ring.magicB = kMagicB;
    }
    initDone = true;
  }
  portEXIT_CRITICAL_SAFE(&ringMux);
  writeBack(&ring, offsetof(Ring, data));
}

void appendRestartMarkerIfPending() {
  if (!restartMarkerPending) return;
  restartMarkerPending = false;
  char marker[80];
  const int len = snprintf(marker, sizeof(marker), "\n=== software restart #%lu (reset reason %d) ===\n",
                           static_cast<unsigned long>(ring.restarts), static_cast<int>(esp_reset_reason()));
  if (len > 0) appendRaw(marker, static_cast<uint32_t>(std::min<int>(len, sizeof(marker) - 1)));
}
}  // namespace

namespace PsramLog {

void append(const char* text, const size_t len) {
  if (len == 0) return;
  ensureInit();
  appendRestartMarkerIfPending();
  appendRaw(text, static_cast<uint32_t>(len));
}

uint32_t oldest() {
  ensureInit();
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  portEXIT_CRITICAL_SAFE(&ringMux);
  return head > kRingBytes ? head - kRingBytes : 0;
}

uint32_t end() {
  ensureInit();
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  portEXIT_CRITICAL_SAFE(&ringMux);
  return head;
}

size_t read(uint32_t& cursor, char* dst, const size_t maxLen) {
  ensureInit();
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t head = ring.head;
  const uint32_t oldestByte = head > kRingBytes ? head - kRingBytes : 0;
  if (cursor < oldestByte || cursor > head) cursor = oldestByte;
  const uint32_t len = std::min<uint32_t>(head - cursor, static_cast<uint32_t>(maxLen));
  const uint32_t pos = cursor % kRingBytes;
  const uint32_t first = std::min(len, kRingBytes - pos);
  memcpy(dst, ring.data + pos, first);
  memcpy(dst + first, ring.data, len - first);
  portEXIT_CRITICAL_SAFE(&ringMux);
  cursor += len;
  return len;
}

}  // namespace PsramLog

#endif
