#pragma once

#include <cstdint>
#include <string>

// Reading stats belong to the reader that recorded them, so a copied card does
// not upload another reader's history as new reading. Each reader keeps its own
// "<name>_<readerId>" copy (readerId = KOReaderSyncClient::readerId(), the stats
// upload identity). Files under the older unqualified names stay on the card
// for older firmware and are read only while this reader has no copy of its
// own, and only when this reader owns them: a stats_origin.bin claim in their
// folder, else in their root (/.crossdink or /.crosspoint, never mixed), names
// the owner; no claim means this reader. docs/file-formats.md has the layout.
namespace StatsStorage {
enum class Owner : uint8_t { Me, Other, Unknown };

// This reader's copy of `legacyPath`: "global_stats.bin" -> "global_stats_<readerId>.bin",
// a folder gets the suffix appended. Empty when the device identity cannot be read.
std::string localPath(const std::string& legacyPath);

// Who recorded the existing unqualified stats file `legacyPath`. Reads only.
// Unknown: an unreadable claim; the caller must neither read nor hide the file.
Owner legacyOwner(const std::string& legacyPath);

// Call before the first save of this reader's copy, with the unqualified file
// that copy replaces ("" when there is none). Records this reader as the owner
// of /.crossdink's unqualified files, and copies the owner into a book folder
// holding unqualified files, so a copy of only that folder keeps it. False:
// do not save (an unreadable claim, so the load could not have read the file,
// or a claim could not be written).
bool beforeFirstSave(const std::string& legacyPath);

// Folders (daily counters, session outbox) have no second slot to fall back
// on, so an older build's folder this reader owns moves into this reader's.
// Where to read `legacyFolder` from: this reader's folder if present, else an
// older build's folder this reader owns. Reads only. Empty without an identity.
std::string folderToRead(const std::string& legacyFolder);
// This reader's folder, after moving in an older build's folder it owns (one
// rename; file by file when both exist after an OTA rollback, keeping this
// reader's copy of a name both hold). Writes: call from save and upload paths.
// Another reader's folder stays as it is. Empty: do not write.
std::string adoptFolder(const std::string& legacyFolder);

// Drops the cached root claims (the card's content may have changed).
void forget();
}  // namespace StatsStorage
