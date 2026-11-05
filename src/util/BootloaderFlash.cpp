#include "BootloaderFlash.h"

#ifndef SIMULATOR
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_flash.h>
#include <esp_ota_ops.h>
#include <hal/efuse_hal.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <cstring>

// The original, behind the --wrap that lets this app verify itself on X4 silicon.
extern "C" esp_err_t __real_bootloader_common_check_efuse_blk_validity(uint32_t min_rev_full, uint32_t max_rev_full);
// Exported by esp_flash_api.c (reads the QE status bit under the flash lock) but left out of esp_flash.h.
extern "C" esp_err_t esp_flash_get_io_mode(esp_flash_t* chip, bool* qe);

namespace bootloader_flash {
namespace {

constexpr size_t HEADER_SIZE = sizeof(esp_image_header_t);
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

  esp_image_header_t hdr;
  memcpy(&hdr, buf, sizeof(hdr));
  if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.chip_id != ESP_CHIP_ID_ESP32S3) {
    return fail(err, errLen, "Not an ESP32-S3 image");
  }
  if (hdr.hash_appended != 1) return fail(err, errLen, "Image has no appended SHA-256");
  // The ROM and the image loader refuse an image built for other silicon revisions (major * 100 + minor).
  const uint32_t rev = efuse_hal_chip_revision();
  if (rev < hdr.min_chip_rev_full || (hdr.max_chip_rev_full != 0xFFFF && rev > hdr.max_chip_rev_full)) {
    snprintf(err, errLen, "Image is for chip rev v%u.%u..v%u.%u, this chip is v%lu.%lu", hdr.min_chip_rev_full / 100,
             hdr.min_chip_rev_full % 100, hdr.max_chip_rev_full / 100, hdr.max_chip_rev_full % 100,
             static_cast<unsigned long>(rev / 100), static_cast<unsigned long>(rev % 100));
    LOG_ERR("BOOTLDR", "%s", err);
    return false;
  }
  uint8_t sha[SHA_SIZE];
  mbedtls_sha256(buf, size - SHA_SIZE, sha, /*is224=*/0);
  if (memcmp(sha, buf + size - SHA_SIZE, SHA_SIZE) != 0) return fail(err, errLen, "SHA-256 mismatch");

  image.size = size;
  image.spiMode = hdr.spi_mode;
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
  const uint32_t blk = efuse_hal_blk_version();
  const uint32_t chip = efuse_hal_chip_revision();
  LOG_INF("BOOT", "idf bootloader app check=%s efuse blk rev=v%lu.%lu chip rev=v%lu.%lu", appAccepted() ? "ok" : "FAIL",
          static_cast<unsigned long>(blk / 100), static_cast<unsigned long>(blk % 100),
          static_cast<unsigned long>(chip / 100), static_cast<unsigned long>(chip % 100));
  // Which bootloader sits at offset 0 (IDF >= 5.1 images carry a description; the factory one may not) and
  // whether the chip's QE bit is set: the two facts a QIO cache depends on.
  esp_bootloader_desc_t desc;
  const esp_err_t rc = esp_ota_get_bootloader_description(nullptr, &desc);
  bool qe = false;
  const esp_err_t qeRc = esp_flash_get_io_mode(esp_flash_default_chip, &qe);
  LOG_INF("BOOT", "bootloader idf=%s built=%s QE=%s", rc == ESP_OK ? desc.idf_ver : "none",
          rc == ESP_OK ? desc.date_time : "?", qeRc == ESP_OK ? (qe ? "1" : "0") : "?");
}

}  // namespace bootloader_flash
#endif  // SIMULATOR
