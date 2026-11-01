#include "BootloaderFlash.h"

#ifndef SIMULATOR
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_app_desc.h>
#include <esp_flash.h>
#include <hal/efuse_hal.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

// The original, behind the --wrap that lets this app verify itself on X4 silicon.
extern "C" esp_err_t __real_bootloader_common_check_efuse_blk_validity(uint32_t min_rev_full, uint32_t max_rev_full);

namespace bootloader_flash {
namespace {

constexpr uint8_t IMAGE_MAGIC = 0xE9;
constexpr uint16_t CHIP_ID_ESP32S3 = 9;
constexpr size_t HEADER_SIZE = 24;
constexpr size_t SHA_SIZE = 32;

bool appAccepted() {
  const esp_app_desc_t* app = esp_app_get_description();
  return __real_bootloader_common_check_efuse_blk_validity(app->min_efuse_blk_rev_full, app->max_efuse_blk_rev_full) ==
         ESP_OK;
}

bool fail(char* err, const size_t errLen, const char* reason) {
  snprintf(err, errLen, "%s", reason);
  LOG_ERR("BOOTLDR", "%s", reason);
  return false;
}

}  // namespace

bool load(uint8_t* buf, Image& image, char* err, const size_t errLen) {
  FsFile file = Storage.open(SD_PATH);
  if (!file) return fail(err, errLen, "No /bootloader.bin on the SD card");
  const size_t size = file.size();
  if (size < HEADER_SIZE + SHA_SIZE + 1 || size > REGION_SIZE) {
    file.close();
    return fail(err, errLen, "Wrong file size");
  }
  const int got = file.read(buf, size);
  file.close();
  if (got != static_cast<int>(size)) return fail(err, errLen, "SD read failed");

  uint16_t chipId;
  memcpy(&chipId, buf + 12, sizeof(chipId));
  if (buf[0] != IMAGE_MAGIC || chipId != CHIP_ID_ESP32S3) return fail(err, errLen, "Not an ESP32-S3 image");
  if (buf[23] != 1) return fail(err, errLen, "Image has no appended SHA-256");
  uint8_t sha[SHA_SIZE];
  mbedtls_sha256(buf, size - SHA_SIZE, sha, /*is224=*/0);
  if (memcmp(sha, buf + size - SHA_SIZE, SHA_SIZE) != 0) return fail(err, errLen, "SHA-256 mismatch");

  image.size = size;
  image.spiMode = buf[2] & 0x0F;
  // A quad bootloader needs QE set in the flash status register; this app's QIO
  // init (esp_flash_init_main) did that, a DIO build never does.
  if (image.spiMode <= 1 && !esp_flash_is_quad_mode(esp_flash_default_chip)) {
    return fail(err, errLen, "Quad bootloader, but this build runs the flash in DIO (QE unset)");
  }
  if (!appAccepted()) return fail(err, errLen, "An IDF 5.5 bootloader would reject this app (efuse blk rev)");
  return true;
}

bool write(const uint8_t* buf, const size_t size, char* err, const size_t errLen) {
  HeapByteBuffer old = makeHeapByteBufferNoThrow(REGION_SIZE);
  if (!old) return fail(err, errLen, "Out of memory");
  if (esp_flash_read(nullptr, old.get(), 0, REGION_SIZE) != ESP_OK) return fail(err, errLen, "Flash read failed");
  if (esp_flash_erase_region(nullptr, 0, REGION_SIZE) != ESP_OK) return fail(err, errLen, "Flash erase failed");
  esp_err_t rc = esp_flash_write(nullptr, buf, 0, size);
  if (rc == ESP_OK) {
    HeapByteBuffer back = makeHeapByteBufferNoThrow(size);
    if (!back || esp_flash_read(nullptr, back.get(), 0, size) != ESP_OK || memcmp(back.get(), buf, size) != 0) {
      rc = ESP_FAIL;
    }
  }
  if (rc == ESP_OK) {
    LOG_INF("BOOTLDR", "wrote %u bytes to 0x0", static_cast<unsigned>(size));
    return true;
  }
  // Put the previous bootloader back so the next boot still has one.
  const bool restored = esp_flash_erase_region(nullptr, 0, REGION_SIZE) == ESP_OK &&
                        esp_flash_write(nullptr, old.get(), 0, REGION_SIZE) == ESP_OK;
  return fail(err, errLen, restored ? "Write failed; old bootloader restored" : "Write failed; RESTORE FAILED");
}

void logAppCheck() {
  const uint32_t rev = efuse_hal_blk_version();
  LOG_INF("BOOT", "idf bootloader app check=%s efuse blk rev=v%lu.%lu", appAccepted() ? "ok" : "FAIL",
          static_cast<unsigned long>(rev / 100), static_cast<unsigned long>(rev % 100));
}

}  // namespace bootloader_flash
#endif  // SIMULATOR
