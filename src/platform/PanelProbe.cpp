#include "PanelProbe.h"

#if !defined(SIMULATOR) && !FREEINK_MCU_C3
#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <esp_system.h>
#include <uzlib.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

namespace PanelProbe {
namespace {

using Diag = freeink::XteinkDisplayProbeDiag;
static_assert(std::is_trivially_copyable<Diag>::value, "the record stores the diag as raw bytes");

constexpr uint32_t RECORD_MAGIC = 0x50505231;  // "PPR1"

// Diag is kept as raw bytes: its member initializers would make static init
// zero an RTC_NOINIT Diag on every boot.
struct Record {
  uint32_t magic;
  uint8_t board;
  uint8_t defaultController;  // the profile's controller before the probe
  uint8_t controller;
  uint8_t variant;
  uint8_t diag[sizeof(Diag)];
  uint32_t crc;
};

// RTC_NOINIT holds garbage after power-on, so every read checks magic and CRC.
RTC_NOINIT_ATTR Record rtcRecord;
Diag restoredDiag;
bool restored = false;

uint32_t checksum(const Record& record) { return uzlib_crc32(&record, offsetof(Record, crc), 0); }

}  // namespace

void resolve() {
  const uint8_t board = static_cast<uint8_t>(BoardConfig::ACTIVE.board);
  const uint8_t defaultController = static_cast<uint8_t>(BoardConfig::ACTIVE.displayController);
  if (esp_reset_reason() == ESP_RST_DEEPSLEEP && rtcRecord.magic == RECORD_MAGIC &&
      rtcRecord.crc == checksum(rtcRecord) && rtcRecord.board == board &&
      rtcRecord.defaultController == defaultController) {
    BoardConfig::ACTIVE.displayController = static_cast<BoardConfig::DisplayController>(rtcRecord.controller);
    BoardConfig::ACTIVE.displayControllerVariant = rtcRecord.variant;
    memcpy(&restoredDiag, rtcRecord.diag, sizeof(Diag));
    restored = true;
    return;
  }

  freeink::applyXteinkDisplayController();
  const Diag& d = freeink::getXteinkDisplayProbeDiag();
  // Record only a decisive answer: an inconclusive probe ran the default
  // controller and must probe again next wake.
  const bool decisive =
      d.valid && (d.promoted || d.verdict == static_cast<uint8_t>(freeink::DisplayControllerVerdict::PrimaryAssumed));
  if (!decisive) {
    rtcRecord.magic = 0;
    return;
  }
  rtcRecord.magic = RECORD_MAGIC;
  rtcRecord.board = board;
  rtcRecord.defaultController = defaultController;
  rtcRecord.controller = static_cast<uint8_t>(BoardConfig::ACTIVE.displayController);
  rtcRecord.variant = BoardConfig::ACTIVE.displayControllerVariant;
  memcpy(rtcRecord.diag, &d, sizeof(Diag));
  rtcRecord.crc = checksum(rtcRecord);
}

const freeink::XteinkDisplayProbeDiag& diag() { return restored ? restoredDiag : freeink::getXteinkDisplayProbeDiag(); }

bool cached() { return restored; }

}  // namespace PanelProbe
#endif
