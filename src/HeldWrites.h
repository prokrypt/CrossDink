#pragma once

#include <cstddef>
#include <cstdint>

// The small store files (state.json, recent.json, settings.json) are held in
// RAM and written to SD only at flush points: sleep, OTA/SD firmware update,
// low battery, USB Drive / File Transfer entry, the remote FLUSH command and
// every restart except a silent one. Firmware builds keep the held copies in
// PSRAM noinit, so a silent restart or a crash reset carries them to the next
// boot (CRC-checked) instead of writing them; power loss loses them, which the
// low-battery write-through bounds. The simulator writes through.
namespace HeldWrites {

enum class Slot : uint8_t { State, Recent, Settings, Count };

// Once PSRAM is up and before the stores load: adopts copies a reset carried.
void begin();

// Holds the serialized file; data is JSON, NUL-terminated at len. Writes now instead when write-through is on or the
// data outgrows the slot. Returns false only when the data was neither held
// nor written.
bool hold(Slot slot, const char* data, size_t len);

// Writes one held file; true when nothing is held or the write landed. A failed
// write stays held for the next flush.
bool flush(Slot slot);

// Writes every held file. Returns how many were written.
int flushAll();

// While any reason is on, every hold writes at once; turning one on flushes.
enum Reason : uint8_t {
  LowBattery = 1,
  ServesFiles = 2,     // USB Drive / File Transfer: a computer shares the card until the screen ends
  FirmwareUpdate = 4,  // the new firmware may not read what PSRAM carries
  Sleeping = 8,        // deep sleep powers PSRAM down
};
void setWriteThrough(Reason reason, bool on);

// The next restart is a silent one: the shutdown handler leaves held files in
// PSRAM for the next boot instead of writing them.
void carryNextRestart();
bool carrying();

}  // namespace HeldWrites
