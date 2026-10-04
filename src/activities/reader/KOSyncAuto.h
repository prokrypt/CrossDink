#pragma once

#include <string>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"

// KOReader Sync > Auto Sync, always Smart Sync and on a background task (no
// screen). At close: push the book's saved progress unless the server is already
// at or past it. At open: fetch the remote progress (never push); the reader asks
// before it moves. Main task only.
namespace kosync_auto {
// Reader open: fetch the book's remote progress when At open is on and credentials exist.
void queuePull(const std::string& epubPath);
// Reader exit: drops a pending fetch; remembers the book for a push when At close is on.
void queue(const std::string& epubPath);
// Reader loop: a finished fetch for this book, once, else null; valid until the
// next fetch starts. method: the hash it matched.
const KOReaderProgress* takePull(const std::string& epubPath, DocumentMatchMethod& method);
// Main loop: starts a queued push or fetch once no Wi-Fi screen is up, at least
// 1.5 s after the book closed or a book opened.
void loop();
// Main loop: true once after a push landed (the toast).
bool takePushed();
// Boot, before a deep-sleep wake reopens the book: with Sync on Wake & Sleep on,
// that open fetches as At open does.
void noteWake();
// Deep sleep, after the reader's exit writes: with Sync on Wake & Sleep on, pushes
// the book the reader just closed and waits for it (bounded).
void syncBeforeSleep();
// Before a Wi-Fi screen or deep sleep takes the radio: waits out the task's
// Wi-Fi start/stop calls; the task then leaves the radio alone.
void yieldRadio();
}  // namespace kosync_auto
