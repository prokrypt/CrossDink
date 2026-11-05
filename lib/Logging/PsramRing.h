#pragma once

#include <esp_cache.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Text ring for .ext_ram_noinit (EXT_RAM_NOINIT_ATTR; needs
// CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY=y). Neither the heap nor the
// startup code touches that segment, so a software restart, panic or OTA
// restart finds the ring as it was; after deep sleep or power loss it is
// random, which the magic words and size reject. PSRAM is mapped by Arduino's
// psramInit() after global constructors, so call adopt() only once
// esp_psram_is_initialized(). The owner serializes adopt()/write() (a
// portMUX) and calls writeBack() after releasing it. PsramLog, BatteryLog.
template <uint32_t N>
struct PsramRing {
  // The S3 MSPI timing tuning writes a 64 B test pattern at physical PSRAM
  // address 0 on every boot (mspi_timing_tuning_configs.h,
  // MSPI_TIMING_PSRAM_TEST_DATA_ADDR), and the noinit segment starts there.
  // Keep the header clear of it.
  char tuningScratch[1024];
  uint32_t magicA;
  uint32_t size;
  uint32_t head;  // total bytes ever written; position = head % N
  uint32_t aux;   // the owner's counter; 0 in a fresh ring
  uint32_t magicB;
  char data[N];

  // Keeps a valid header (true) or starts an empty ring (false).
  bool adopt(const uint32_t a, const uint32_t b) {
    if (magicA == a && magicB == b && size == N) return true;
    magicA = a;
    size = N;
    head = 0;
    aux = 0;
    magicB = b;
    return false;
  }

  // Keeps only the tail of text longer than the ring.
  void write(const char* text, uint32_t len) {
    if (len > N) {
      text += len - N;
      len = N;
    }
    const uint32_t pos = head % N;
    const uint32_t first = std::min(len, N - pos);
    memcpy(data + pos, text, first);
    memcpy(data, text + first, len - first);
    head += len;
  }

  // Copies [from, from + len); the caller keeps that inside the last N bytes.
  void copyOut(const uint32_t from, char* dst, const uint32_t len) const {
    const uint32_t pos = from % N;
    const uint32_t first = std::min(len, N - pos);
    memcpy(dst, data + pos, first);
    memcpy(dst + first, data, len - first);
  }

  // PSRAM sits behind a write-back cache. Push bytes a write() added (from
  // startHead) and the header out right away, so a panic or restart that
  // skips the cache flush still leaves them in PSRAM.
  void writeBack(const uint32_t startHead, uint32_t len) {
    len = std::min(len, N);
    const uint32_t pos = startHead % N;
    const uint32_t first = std::min(len, N - pos);
    sync(data + pos, first);
    if (len > first) sync(data, len - first);
    writeBackHeader();
  }

  void writeBackHeader() { sync(&magicA, offsetof(PsramRing, data) - offsetof(PsramRing, magicA)); }

  static void sync(const void* addr, const size_t len) {
    esp_cache_msync(const_cast<void*>(addr), len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
  }
};
