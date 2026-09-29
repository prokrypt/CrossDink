#include "DeviceSecurity.h"

#include <cstdio>

#ifndef SIMULATOR
#include <sdkconfig.h>
#endif

#if !defined(SIMULATOR) && defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_flash_encrypt.h>
#include <esp_secure_boot.h>
#endif

namespace DeviceSecurity {

namespace {
State gState;

void readOnce() {
  if (gState.read) return;
#if !defined(SIMULATOR) && defined(CONFIG_IDF_TARGET_ESP32S3)
  switch (esp_get_flash_encryption_mode()) {
    case ESP_FLASH_ENC_MODE_RELEASE:
      gState.flashEnc = "release";
      break;
    case ESP_FLASH_ENC_MODE_DEVELOPMENT:
      gState.flashEnc = "dev";
      break;
    default:
      gState.flashEnc = "off";
      break;
  }
  gState.secureBoot = esp_secure_boot_enabled();
  gState.usbSerialJtagDisabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_SERIAL_JTAG);
  // SOFT_DIS_JTAG is a 3-bit field: an odd number of set bits disables JTAG.
  uint8_t softDis = 0;
  esp_efuse_read_field_blob(ESP_EFUSE_SOFT_DIS_JTAG, &softDis, 3);
  const bool softDisabled = (__builtin_popcount(softDis & 0x7) & 1) != 0;
  gState.jtagDisabled = esp_efuse_read_field_bit(ESP_EFUSE_DIS_USB_JTAG) ||
                        esp_efuse_read_field_bit(ESP_EFUSE_DIS_PAD_JTAG) || softDisabled;
  if (esp_efuse_read_field_bit(ESP_EFUSE_DIS_DOWNLOAD_MODE)) {
    gState.download = "off";
  } else if (esp_efuse_read_field_bit(ESP_EFUSE_ENABLE_SECURITY_DOWNLOAD)) {
    gState.download = "secure";
  }
  gState.locked = gState.flashEnc[0] == 'r' || gState.secureBoot || gState.download[1] == 'f' /* off */ ||
                  gState.usbSerialJtagDisabled;
  gState.read = true;
#endif
}
}  // namespace

const State& get() {
  readOnce();
  return gState;
}

size_t format(char* buf, const size_t size) {
  const State& s = get();
  const int n =
      snprintf(buf, size, "fenc=%s sboot=%d usbjtag=%s jtag=%s dl=%s locked=%d", s.flashEnc, s.secureBoot ? 1 : 0,
               s.usbSerialJtagDisabled ? "off" : "on", s.jtagDisabled ? "off" : "on", s.download, s.locked ? 1 : 0);
  if (n <= 0 || size == 0) return 0;
  return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}

}  // namespace DeviceSecurity
