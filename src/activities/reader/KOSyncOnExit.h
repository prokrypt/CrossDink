#pragma once

#include <string>

// KOReader Sync > Sync on Book Exit: after a book closes, push its saved
// progress to the sync server on a background task (no screen, no pull).
// Main task only.
namespace kosync_on_exit {
// Reader exit: remember the book when the setting is on and credentials exist.
void queue(const std::string& epubPath);
// Main loop: starts the push once a non-reader, non-Wi-Fi screen is up.
void loop();
// Before a Wi-Fi screen or deep sleep takes the radio: waits out the push's
// Wi-Fi start/stop calls; the push then leaves the radio alone.
void yieldRadio();
}  // namespace kosync_on_exit
