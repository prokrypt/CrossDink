#include "PsramLog.h"

#if CROSSDINK_PSRAM_LOG && !defined(SIMULATOR)

#include <esp_attr.h>
#include <esp_cache.h>
#include <esp_psram.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <sdkconfig.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if !CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#error "CROSSDINK_PSRAM_LOG needs CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y"
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
  // The S3 MSPI timing tuning writes a 64 B test pattern at physical PSRAM
  // address 0 on every boot (mspi_timing_tuning_configs.h,
  // MSPI_TIMING_PSRAM_TEST_DATA_ADDR), and the noinit segment starts there.
  // Keep the header clear of it.
  char tuningScratch[1024];
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

// PSRAM sits behind a write-back cache. Push the new bytes out right away so
// a panic or restart that skips the cache flush still leaves them in PSRAM.
void writeBack(const void* addr, const size_t len) {
  esp_cache_msync(const_cast<void*>(addr), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void writeBackHeader() { writeBack(&ring.magicA, offsetof(Ring, data) - offsetof(Ring, magicA)); }

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
  writeBackHeader();
}

void appendRaw(const char* text, const uint32_t len) {
  portENTER_CRITICAL_SAFE(&ringMux);
  const uint32_t startHead = ring.head;
  writeLocked(text, len);
  portEXIT_CRITICAL_SAFE(&ringMux);
  writeBackRange(startHead, std::min(len, kRingBytes));
}

// Header as found at boot, reported once in the first line this boot writes.
uint32_t foundMagicA = 0, foundMagicB = 0, foundSize = 0, foundHead = 0;
bool headerKept = false;
bool bootLinePending = false;

// Belt and braces for the per-append write-back: flush the header once more
// on the way down through esp_restart().
void flushHeaderOnRestart() { writeBackHeader(); }

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
    headerKept = ring.magicA == kMagicA && ring.magicB == kMagicB && ring.size == kRingBytes;
    if (headerKept) {
      ring.restarts++;
    } else {
      ring.magicA = kMagicA;
      ring.size = kRingBytes;
      ring.head = 0;
      ring.restarts = 0;
      ring.magicB = kMagicB;
    }
    bootLinePending = true;
    initDone = true;
    didInit = true;
  }
  portEXIT_CRITICAL_SAFE(&ringMux);
  if (didInit) {
    writeBackHeader();
    esp_register_shutdown_handler(flushHeaderOnRestart);
  }
  return true;
}

void appendBootLineIfPending() {
  if (!bootLinePending) return;
  bootLinePending = false;
  char line[200];
  const int len = snprintf(line, sizeof(line),
                           "\n=== boot: PSRAM log %s, restart #%lu, reset reason %d, ring@%p, found magic %08lx/%08lx "
                           "size %lu head %lu ===\n",
                           headerKept ? "kept" : "reset", static_cast<unsigned long>(ring.restarts),
                           static_cast<int>(esp_reset_reason()), static_cast<void*>(&ring),
                           static_cast<unsigned long>(foundMagicA), static_cast<unsigned long>(foundMagicB),
                           static_cast<unsigned long>(foundSize), static_cast<unsigned long>(foundHead));
  if (len > 0) appendRaw(line, static_cast<uint32_t>(std::min<int>(len, sizeof(line) - 1)));
}
}  // namespace

namespace PsramLog {

void append(const char* text, const size_t len) {
  if (len == 0 || !ensureInit()) return;
  appendBootLineIfPending();
  appendRaw(text, static_cast<uint32_t>(len));
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
