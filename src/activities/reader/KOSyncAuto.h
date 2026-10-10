#pragma once

#include <cstdint>
#include <string>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"

namespace EpubReaderUtils {
struct Progress;
}

// KOReader Sync > Auto Sync, always Smart Sync and on a background task (no
// screen). At close: push the book's saved progress unless the server is already
// at or past it, or the book closed at the page it opened at (no radio). At open: fetch the remote progress (never
// push); the reader asks before it moves. Main task only.
namespace kosync_auto {
// Reader open: fetch the book's remote progress when At open is on and credentials exist.
// spineIndex/pageNumber/pageCount (0 if unknown): where the book opened; a close
// or sleep push never goes out from an earlier position.
void queuePull(const std::string& epubPath, int spineIndex, int pageNumber, int pageCount);
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
// Boot, before a deep-sleep wake reopens the book: when the sleep push failed,
// that open pushes too (its fetch comes from At open, as on any open).
void noteWake();
enum class PushOutcome : uint8_t { Pushed, Same, ServerAhead, Failed };
// Deep sleep, open reader: false when the saved position is not past where the
// book opened this boot and no earlier push of it is owed, so sleep skips the
// push and its toasts.
bool movedSinceOpen(const std::string& epubPath, const EpubReaderUtils::Progress& saved);
// Sync on Wake & Sleep is on, Auto Sync has At close, credentials exist and no
// Wi-Fi screen is up.
bool wantsSleepPush();
// Deep sleep, before the sleep screen: pushBegin() starts the push (never moving
// the server back) without blocking, so the caller can draw its toast while Wi-Fi
// joins (a job already running is waited out in pushFinish(), under the toast).
// pushFinish() then blocks until done, at most 30 s in all from pushBegin().
// A failure arms the push on the next wake. pushBegin() false means nothing
// started (already armed for the wake): skip pushFinish().
// readerFlushed: the position was just saved by an open reader (else the book is
// pendingPushPath()'s, and a push of it already running is finished, not redone).
bool pushBegin(std::string epubPath, bool readerFlushed);
PushOutcome pushFinish();
// A close push queued or running (not yet landed), when no Wi-Fi screen is up.
std::string pendingPushPath();
// Before a Wi-Fi screen or deep sleep takes the radio: waits out the task's
// Wi-Fi start/stop calls; the task then leaves the radio alone.
void yieldRadio();
}  // namespace kosync_auto
