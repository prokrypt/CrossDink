#pragma once

#include <cstddef>
#include <cstdint>

// Goodies > Flash bootloader: write /bootloader.bin from the SD card to flash
// offset 0 (the second-stage bootloader, which OTA never touches). Hardware only.
namespace bootloader_flash {

constexpr const char* SD_PATH = "/bootloader.bin";
constexpr size_t REGION_SIZE = 0x8000;  // 0x0 .. partition table

struct Image {
  size_t size = 0;
  uint8_t spiMode = 0;  // header byte 2: 0 qio, 1 qout, 2 dio, 3 dout
};

// Reads and validates the SD file into `buf` (REGION_SIZE bytes): ESP32-S3 image
// header, appended SHA-256, and that an ESP-IDF 5.5 bootloader would accept the
// running app (its efuse block revision check) and a quad bootloader finds QE set.
// False with a short reason in `err`.
bool load(uint8_t* buf, Image& image, char* err, size_t errLen);

// Erases offset 0..REGION_SIZE, writes the image and reads it back; on a
// mismatch the previous bootloader is written back. False with a reason in `err`.
bool write(const uint8_t* buf, size_t size, char* err, size_t errLen);

// One [BOOT] line: whether the IDF bootloader check passes for this app and the
// efuse block revision it saw.
void logAppCheck();

}  // namespace bootloader_flash
