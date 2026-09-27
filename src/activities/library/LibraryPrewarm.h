#pragma once

#include <cstdint>

// Builds the Library index in the background while Home is shown, so the first
// Library visit after a boot or reset opens the existing index instead of
// walking the card behind a "Reading your books" popup.
//
// Home drives it: tick() starts the build as soon as Home is up, pauses it on
// input and resumes it after a short quiet spell, so the walk only borrows the
// card and CPU while nobody is using them. A paused build keeps its place; it
// does not start over.
namespace LibraryPrewarm {

// Called from Home's loop. `idle` is true until the first input and again
// shortly after the last one; false pauses a running build at its next
// directory entry.
void tick(bool idle);

// Pauses a running build at its next directory entry (Home saw input or is
// rendering). Safe to call from any task.
void pause();

// Home is leaving. With `handOffToLibrary` the build keeps running for the
// Library to finish; otherwise it is stopped and joined before returning, so no
// file or heap stays in use behind the next screen.
void stop(bool handOffToLibrary);

// True while a background build is in flight (running or paused).
bool active();

// True while a build is running rather than paused. Home holds off auto sleep
// during these bursts; the build task keeps the CPU at full speed itself.
bool working();

// Library entry: lets a handed-off build run unpaused and waits up to `waitMs`
// for it. Returns true once nothing is running; Storage.libraryScanCurrent()
// then says whether it left a current index.
bool finishForLibrary(uint32_t waitMs = UINT32_MAX);

}  // namespace LibraryPrewarm
