#include "PackedAsset.h"

#include <Logging.h>

#include <cstring>

#include "InflateStream.h"

#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR) && !defined(CROSSDINK_MEMORY_TEST)
#include <esp_timer.h>
#endif

namespace {
int64_t nowUs() {
#if defined(ARDUINO_ARCH_ESP32) && !defined(SIMULATOR) && !defined(CROSSDINK_MEMORY_TEST)
  return esp_timer_get_time();
#else
  return 0;
#endif
}
}  // namespace

HeapByteBuffer inflatePackedAsset(const PackedAsset& asset, const char* tag, const PackedAssetFallback fallback,
                                  const size_t headroom) {
  if (!asset.data || asset.rawSize == 0) return {};
  const size_t total = headroom + asset.rawSize;

  // Runtime-sized and long-lived (until the asset is switched out), so neither
  // stack nor static storage fits. PSRAM keeps internal RAM for DMA/stacks.
  HeapByteBuffer out;
  if (psramHeapAvailable()) {
    out = makePsramByteBufferNoThrow(total);
    if (!out && fallback == PackedAssetFallback::PsramThenDefault) out = makeHeapByteBufferNoThrow(total);
  } else {
    out = makeHeapByteBufferNoThrow(total);
  }
  if (!out) {
    LOG_ERR(tag, "OOM inflating packed asset (%u bytes)", unsigned(total));
    return {};
  }
  if (headroom) memset(out.get(), 0, headroom);

  const int64_t startUs = nowUs();
  // One-shot mode: the output buffer holds the whole result, so no 32 KB
  // window is needed; only tinfl's ~11 KB state is transient.
  InflateStream stream;
  if (!stream.init(false)) {
    LOG_ERR(tag, "No inflate state for packed asset");
    return {};
  }
  stream.setSource(asset.data, asset.packedSize);
  if (!stream.read(out.get() + headroom, asset.rawSize)) {
    LOG_ERR(tag, "Corrupt packed asset (%u -> %u bytes)", unsigned(asset.packedSize), unsigned(asset.rawSize));
    return {};
  }
  LOG_DBG(tag, "Inflated %u -> %u bytes in %ld us", unsigned(asset.packedSize), unsigned(asset.rawSize),
          static_cast<long>(nowUs() - startUs));
  return out;
}
