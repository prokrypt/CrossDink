#pragma once

#include <cstdint>
#include <string>

// Mirrors the open book's newest reading position in RTC memory. RTC_NOINIT
// survives panics, watchdog resets, ESP.restart() and deep sleep, but not power
// loss, so a position that has not reached progress.bin yet is only lost when
// the battery dies or is disconnected. That lets the reader write progress.bin
// far less often (see ReaderProgressSaveDebouncer::setShadowed).
//
// Each update is a RAM store; nothing here touches the SD card except
// recoverPending(), which replays a position a reset kept off the card.
namespace ReaderProgressShadow {

enum class Kind : uint8_t { None = 0, Epub = 1, Txt = 2, Xtc = 3 };

// Replays an unsaved position into its book's progress.bin, then clears the
// record. setup() calls it once the SD card mounts, before anything reads
// progress; bind() calls it too in case that replay failed.
void recoverPending();

// Starts mirroring a book. Returns false when the cache path does not fit, in
// which case nothing is mirrored and the caller keeps the unshadowed intervals.
bool bind(Kind kind, const std::string& cachePath);

// Stops mirroring after the reader flushed its position on exit. Keeps the
// record when that flush failed so the next boot can replay it.
void unbind(bool flushed);

// Records a position the SD card does not have yet. For EPUB, `position` is
// spine << 16 | page and `metadata` the chapter page count; TXT passes the page
// and its file offset; XTC passes the page and 0.
void notePending(uint32_t position, uint32_t metadata);

// Marks the record clean once progress.bin holds this exact position.
void notePersisted(uint32_t position, uint32_t metadata);

}  // namespace ReaderProgressShadow
