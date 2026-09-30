// wolfSSL heap hooks, selected by XMALLOC_USER (scripts/patch_wolfssl.py).
#if defined(FREEINK_NET_WOLFSSL) && defined(ESP_PLATFORM)

#include <esp_heap_caps.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/types.h>

#include <cstdlib>

// A TLS session outlives its connection: SecureClient keeps the last one for
// resumption, past the OPDS screen and the Wi-Fi session. Blocks under
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (1 KB) land in internal RAM wherever the
// heap has room at handshake time, which during parallel preloads is inside
// the one large block the Wi-Fi exit gate needs; the kept session then splits
// it (0929d: 63 KB -> 41 KB, exit by restart). Sessions are plain CPU data,
// never DMA or ISR, so they go to PSRAM. Everything else keeps plain malloc.
namespace {
bool isSession(const int type) { return type == DYNAMIC_TYPE_SESSION || type == DYNAMIC_TYPE_SESSION_TICK; }
}  // namespace

extern "C" void* XMALLOC(size_t n, void* heap, int type) {
  (void)heap;
  if (!isSession(type)) return malloc(n);
  return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                 MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}

extern "C" void* XREALLOC(void* p, size_t n, void* heap, int type) {
  (void)heap;
  if (!isSession(type)) return realloc(p, n);
  return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                  MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}

extern "C" void XFREE(void* p, void* heap, int type) {
  (void)heap;
  (void)type;
  free(p);
}

#endif
