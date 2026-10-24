#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

// Reader exit files (progress.bin and its backup slot, progress_percent.bin)
// and a book's reader_settings.bin are held the same way, a few small files by
// path. hold false writes now and
// drops a held copy of path, which that write supersedes; hold true holds it,
// or writes now under write-through, with the table full or data too long.
// Data equal to the held copy (else the card's file) is not rewritten. Returns
// false only when the data was neither held nor written.
bool saveFile(const char* path, const uint8_t* data, size_t len, bool hold);
// The held copy of path: copies up to cap bytes and returns its length, or -1
// when path is not held.
int heldFile(const char* path, uint8_t* out, size_t cap);
// heldFile(), else up to cap bytes of the card's file; -1 when neither opens.
int readFile(const char* module, const std::string& path, uint8_t* out, size_t cap);
// Forgets a held copy of path without writing it (the caller removes the file).
void dropFile(const char* path);
// Writes every held file (flushAll() does too). A file whose folder is gone
// (cache cleared, book deleted) is dropped.
void flushFiles();

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
