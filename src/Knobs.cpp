#include <Knobs.h>

#if CROSSDINK_GOODIES

#include <FreeInkDisplay.h>
#include <HalStorage.h>
#include <InputManager.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <uzlib.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>

#include "platform/PinMon.h"

Knobs KNOBS;

namespace knobs {
const Info INFO[] = {
#define X(group, id, type, def, min, max, step, unit) {group, #id, def, min, max, step, unit},
#include "Knobs.def"
#undef X
};

namespace {
constexpr char PATH[] = "/.crosspoint/knobs.json";
constexpr char BAD_PATH[] = "/.crosspoint/knobs.bad.json";
// Fields have their constants' types, so each gets its own accessor pair.
int32_t (*const GET[])() = {
#define X(group, id, ...) [] { return static_cast<int32_t>(KNOBS.id); },
#include "Knobs.def"
#undef X
};
void (*const PUT[])(int32_t) = {
#define X(group, id, type, ...) [](const int32_t v) { KNOBS.id = static_cast<type>(v); },
#include "Knobs.def"
#undef X
};

// Boots since the device last stayed up 30 s. RTC_NOINIT survives crashes,
// restarts and deep sleep, but holds garbage after power-on, and CrossPoint or
// CrossInk flashed over USB and back may leave their own data at this address,
// so the count lives in one sealed record (ReaderProgressShadow's pattern) and
// any failed check starts it from zero.
struct BootRecord {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint32_t badBoots;
  uint32_t crc;
};
RTC_NOINIT_ATTR BootRecord bootRecord;
constexpr uint32_t BOOT_MAGIC = 0x43444B42;  // "CDKB", CrossDink only
constexpr uint16_t BOOT_VERSION = 1;
constexpr uint32_t MAX_BAD_BOOTS = 3;

uint32_t bootCrc() { return uzlib_crc32(&bootRecord, offsetof(BootRecord, crc), 0); }

bool bootRecordValid() {
  return bootRecord.magic == BOOT_MAGIC && bootRecord.version == BOOT_VERSION &&
         bootRecord.size == sizeof(BootRecord) && bootRecord.crc == bootCrc() && bootRecord.badBoots <= MAX_BAD_BOOTS;
}

void setBadBoots(const uint32_t n) {
  bootRecord = {BOOT_MAGIC, BOOT_VERSION, sizeof(BootRecord), std::min(n, MAX_BAD_BOOTS + 1), 0};
  bootRecord.crc = bootCrc();
}
constexpr uint32_t GOOD_BOOT_MS = 30000;
bool bootCleared = false;

int32_t clampSnap(const Info& k, const int32_t v) {
  const int32_t c = std::clamp(v, k.min, k.max);
  return std::min(k.max, k.min + (c - k.min + k.step / 2) / k.step * k.step);
}

// The SDK keeps its own copies: push them after every change.
void apply() {
  freeink::Uc8179Tuning d;
  d.paintFrames = KNOBS.paintFrames;
  d.coldDivisor = KNOBS.coldDivisor;
  d.heldRedriveFrames = KNOBS.heldRedriveFrames;
  d.coldC = KNOBS.coldC;
  d.tempPeriodMs = KNOBS.tempPeriodMs;
  d.tempRefreshPeriodMs = KNOBS.tempRefreshPeriodMs;
  d.tempMaxAgeMs = KNOBS.tempMaxAgeMs;
  freeink::setUc8179Tuning(d);
  InputManager::Tuning t;
  t.homeKeyLongPressMs = KNOBS.homeKeyLongMs;
  t.confirmBackHoldMs = KNOBS.confirmBackHoldMs;
  t.confirmPowerHoldMs = KNOBS.confirmPowerHoldMs;
  t.twoButtonHoldMs = KNOBS.twoButtonHoldMs;
  t.touchIrqPulseMs = KNOBS.touchIrqPulseMs;
  t.touchTapSlopPx = KNOBS.tapSlopPx;
  t.touchSwipeMinPx = KNOBS.swipeMinPx;
  t.touchSwipeMaxMs = KNOBS.swipeMaxMs;
  t.touchMultiSwipeMaxMs = KNOBS.multiSwipeMaxMs;
  t.touchMultiSeparationSlopPx = KNOBS.multiSeparationSlopPx;
  t.touchLongPressMs = KNOBS.touchLongPressMs;
  t.touchContactJumpPx = KNOBS.contactJumpPx;
  InputManager::setTuning(t);
  PinMon::setEnabled(KNOBS.pinMon != 0);
}

std::atomic<bool> dirty{false};

bool save() {
  JsonDocument doc;
  for (int i = 0; i < COUNT; ++i) {
    if (get(i) != INFO[i].def) doc[INFO[i].id] = get(i);
  }
  if (doc.size() == 0) return !Storage.exists(PATH) || Storage.remove(PATH);
  return PersistableStoreBase::writeDocToFileAtomically(PATH, doc);
}
}  // namespace

int32_t get(const int index) { return GET[index](); }

int find(const char* id) {
  for (int i = 0; i < COUNT; ++i) {
    if (strcmp(INFO[i].id, id) == 0) return i;
  }
  return -1;
}

int32_t set(const int index, const int32_t value, const bool persist) {
  const int32_t v = clampSnap(INFO[index], value);
  PUT[index](v);
  apply();
  LOG_INF("KNOB", "%s = %ld", INFO[index].id, static_cast<long>(v));
  if (persist) dirty = true;
  return v;
}

void resetAll() {
  KNOBS = Knobs{};
  apply();
  dirty = true;  // save() deletes knobs.json when every knob is at its default
  LOG_INF("KNOB", "all knobs reset");
}

void flush() {
  if (dirty.exchange(false) && !save()) LOG_ERR("KNOB", "knobs.json not saved");
}

void load(const bool skipFile) {
  // Only crashes and power cycles count: a wake from sleep or an intentional
  // restart (Wi-Fi entry/exit, OTA) ended the last boot cleanly.
  const esp_reset_reason_t reason = esp_reset_reason();
  const char* record = !bootRecordValid() ? "invalid" : "valid";
  if (!bootRecordValid() || reason == ESP_RST_POWERON || reason == ESP_RST_DEEPSLEEP || reason == ESP_RST_SW) {
    if (bootRecordValid()) record = "reset";
    setBadBoots(0);
  }
  // One line per boot: record state, earlier boots that never reached 30 s
  // (this boot is not one of them yet), what happened to the file.
  const uint32_t unfinished = bootRecord.badBoots;
  const auto logBoot = [&](const char* file) {
    LOG_INF("KNOB", "boot: record %s (reset %d), unfinished boots before this %lu, knobs.json %s", record,
            static_cast<int>(reason), static_cast<unsigned long>(unfinished), file);
  };
  if (skipFile) {
    // Safe boot: the file stays; the next boot without Back loads it again.
    logBoot("bypassed (Back held), defaults in use");
    apply();
    return;
  }
  setBadBoots(bootRecord.badBoots + 1);
  if (bootRecord.badBoots > MAX_BAD_BOOTS) {
    // A knob may be what keeps the device from staying up: set the file aside.
    LOG_ERR("KNOB", "%lu boots without 30 s up: knobs.json moved to knobs.bad.json, defaults in use",
            static_cast<unsigned long>(MAX_BAD_BOOTS));
    if (Storage.exists(BAD_PATH)) Storage.remove(BAD_PATH);
    if (Storage.exists(PATH) && !Storage.rename(PATH, BAD_PATH)) Storage.remove(PATH);
    setBadBoots(0);
    logBoot("set aside as knobs.bad.json");
    apply();
    return;
  }
  JsonDocument doc;
  if (PersistableStoreBase::readDocFromFile(PATH, doc)) {
    int loaded = 0;
    for (int i = 0; i < COUNT; ++i) {
      const JsonVariantConst v = doc[INFO[i].id];
      if (!v.is<int32_t>()) continue;
      PUT[i](clampSnap(INFO[i], v.as<int32_t>()));
      LOG_INF("KNOB", "%s = %ld (default %ld)", INFO[i].id, static_cast<long>(get(i)), static_cast<long>(INFO[i].def));
      ++loaded;
    }
    char file[32];
    snprintf(file, sizeof(file), "loaded, %d knobs", loaded);
    logBoot(file);
  } else {
    logBoot("none, defaults in use");
  }
  apply();
}

void loop() {
  if (bootCleared || millis() < GOOD_BOOT_MS) return;
  bootCleared = true;
  setBadBoots(0);
}
}  // namespace knobs

#endif  // CROSSDINK_GOODIES
