#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_attr.h>

#include "OpdsPageCache.h"

// Kept apart from OpdsPageCache.cpp, which the native tests build without the HAL.
namespace {
std::unique_ptr<OpdsPageCache> sHandoff;
std::string sFocusUrl;

// Opening a server from the list usually reboots into a lean network boot, which
// empties PSRAM: its prefetched root page rides the reboot on the SD card.
constexpr const char* RESTART_PAGE = "/.crossdink/opds-restart.bin";
constexpr uint32_t RESTART_MAGIC = 0x4F505253;  // "OPRS"
// Set only when the file was written right before a software reboot, so a file
// left by a power cut is never read as fresh.
RTC_NOINIT_ATTR uint32_t restartPageMagic;
}  // namespace

namespace opds_page_cache_handoff {
void give(std::unique_ptr<OpdsPageCache> cache, std::string focusUrl) {
  sHandoff = std::move(cache);
  sFocusUrl = std::move(focusUrl);
}

std::unique_ptr<OpdsPageCache> take() {
  sFocusUrl.clear();
  return std::move(sHandoff);
}

void persistForRestart() {
  restartPageMagic = 0;
  if (!sHandoff || sFocusUrl.empty() || sFocusUrl.size() > UINT16_MAX) return;
  const OpdsPageBuffer* page = sHandoff->find(sFocusUrl);
  if (!page || page->empty()) return;
  HalFile f;
  if (!Storage.openFileForWrite("OPDS", RESTART_PAGE, f)) return;
  const uint16_t urlLen = static_cast<uint16_t>(sFocusUrl.size());
  bool ok = f.write(&urlLen, sizeof(urlLen)) == sizeof(urlLen) && f.write(sFocusUrl.data(), urlLen) == urlLen &&
            f.write(page->data(), page->size()) == page->size();
  ok = f.close() && ok;
  if (!ok) {
    LOG_ERR("OPDS", "Could not keep the root page for the reboot");
    Storage.remove(RESTART_PAGE);
    return;
  }
  restartPageMagic = RESTART_MAGIC;
  LOG_INF("OPDS", "Kept the root page for the reboot (%zu bytes)", page->size());
}

void restoreAfterRestart(OpdsPageCache& cache) {
  if (restartPageMagic != RESTART_MAGIC) return;
  restartPageMagic = 0;
  HalFile f;
  if (!Storage.openFileForReadIfPresent("OPDS", RESTART_PAGE, f)) return;
  uint16_t urlLen = 0;
  std::string url;
  bool ok = f.read(&urlLen, sizeof(urlLen)) == sizeof(urlLen) && urlLen > 0;
  if (ok) {
    url.resize(urlLen);
    ok = f.read(&url[0], urlLen) == urlLen;
  }
  OpdsPageBuffer page(MemoryPool::Psram, OPDS_PAGE_MAX_BYTES);
  auto chunk = makeUniqueNoThrow<uint8_t[]>(4096);  // above 1 KB: PSRAM
  ok = ok && chunk;
  int r;
  while (ok && (r = f.read(chunk.get(), 4096)) > 0) ok = page.append(chunk.get(), static_cast<size_t>(r));
  f.close();
  Storage.remove(RESTART_PAGE);
  if (!ok || page.empty()) {
    LOG_ERR("OPDS", "Could not restore the root page after the reboot");
    return;
  }
  const size_t size = page.size();
  if (cache.store(url, std::move(page), true, millis())) {
    LOG_INF("OPDS", "Restored the root page after the reboot (%zu bytes)", size);
  }
}
}  // namespace opds_page_cache_handoff
