// wolfSSL heap hooks, selected by XMALLOC_USER (scripts/patch_wolfssl.py).
#if defined(FREEINK_NET_WOLFSSL) && defined(ESP_PLATFORM)

#include <esp_heap_caps.h>
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/types.h>

#include <cstdlib>

// wolfSSL here is software crypto (no WOLFSSL_ESP32 HW, no DMA): every block is
// plain CPU data, so all of it prefers PSRAM. Blocks under
// CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (1 KB) would otherwise land in internal
// RAM wherever the heap has room at handshake time, which during parallel OPDS
// preloads is inside the one large block the Wi-Fi exit gate needs. The kept
// TLS session (resumption, outlives its connection) split it that way (0929d:
// 63 KB -> 41 KB, exit by restart); handshake and record churn does the same.
extern "C" void* XMALLOC(size_t n, void* heap, int type) {
  (void)heap;
  (void)type;
  return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                 MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}

extern "C" void* XREALLOC(void* p, size_t n, void* heap, int type) {
  (void)heap;
  (void)type;
  return heap_caps_realloc_prefer(p, n, 2, MALLOC_CAP_DEFAULT | MALLOC_CAP_SPIRAM,
                                  MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL);
}

extern "C" void XFREE(void* p, void* heap, int type) {
  (void)heap;
  (void)type;
  free(p);
}

#endif
