#include "PanelProbe.h"

#if !defined(SIMULATOR) && !FREEINK_MCU_C3
#include <Arduino.h>
#include <BoardConfig.h>
#include <Logging.h>
#include <nvs.h>

#include <cstring>
#include <type_traits>

namespace PanelProbe {
namespace {

using Diag = freeink::XteinkDisplayProbeDiag;
static_assert(std::is_trivially_copyable<Diag>::value, "the record stores the diag as raw bytes");

constexpr char NVS_NAMESPACE[] = "panel";
constexpr char NVS_KEY[] = "probe";
constexpr uint8_t RECORD_VERSION = 1;

struct Record {
  uint8_t version;
  uint8_t board;
  uint8_t defaultController;  // the profile's controller before the probe
  uint8_t controller;
  uint8_t variant;
  uint8_t reserved[3];
  uint64_t mac;  // eFuse MAC: a flash image copied from another unit carries that unit's record
  Diag diag;
};

Diag restoredDiag;
bool restored = false;

bool load(Record& record) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  size_t size = sizeof(record);
  const esp_err_t err = nvs_get_blob(handle, NVS_KEY, &record, &size);
  nvs_close(handle);
  return err == ESP_OK && size == sizeof(record);
}

void store(const Record* record) {
  nvs_handle_t handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
    LOG_ERR("PANEL", "probe record: nvs_open failed");
    return;
  }
  const esp_err_t err =
      record ? nvs_set_blob(handle, NVS_KEY, record, sizeof(*record)) : nvs_erase_key(handle, NVS_KEY);
  if (err == ESP_OK) nvs_commit(handle);
  nvs_close(handle);
}

}  // namespace

void resolve() {
  const uint8_t board = static_cast<uint8_t>(BoardConfig::ACTIVE.board);
  const uint8_t defaultController = static_cast<uint8_t>(BoardConfig::ACTIVE.displayController);
  const uint64_t mac = ESP.getEfuseMac();
  Record record;
  if (load(record) && record.version == RECORD_VERSION && record.board == board &&
      record.defaultController == defaultController && record.mac == mac) {
    BoardConfig::ACTIVE.displayController = static_cast<BoardConfig::DisplayController>(record.controller);
    BoardConfig::ACTIVE.displayControllerVariant = record.variant;
    restoredDiag = record.diag;
    restored = true;
    return;
  }

  freeink::applyXteinkDisplayController();
  const Diag& d = freeink::getXteinkDisplayProbeDiag();
  // Record only a decisive answer: an inconclusive probe ran the default
  // controller and must probe again next boot.
  const bool decisive =
      d.valid && (d.promoted || d.verdict == static_cast<uint8_t>(freeink::DisplayControllerVerdict::PrimaryAssumed));
  if (!decisive) {
    store(nullptr);
    return;
  }
  record = {};
  record.version = RECORD_VERSION;
  record.board = board;
  record.defaultController = defaultController;
  record.controller = static_cast<uint8_t>(BoardConfig::ACTIVE.displayController);
  record.variant = BoardConfig::ACTIVE.displayControllerVariant;
  record.mac = mac;
  record.diag = d;
  store(&record);
}

const freeink::XteinkDisplayProbeDiag& diag() { return restored ? restoredDiag : freeink::getXteinkDisplayProbeDiag(); }

bool cached() { return restored; }

}  // namespace PanelProbe
#endif
