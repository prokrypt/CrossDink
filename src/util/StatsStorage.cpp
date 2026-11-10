#include "StatsStorage.h"

#include <HalStorage.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sync/SyncFiles.h"

namespace {
constexpr char NATIVE_ROOT[] = "/.crossdink";
constexpr char TWIN_ROOT[] = "/.crosspoint";
constexpr char CLAIM[] = "/stats_origin.bin";
// Claim payload: u8 version (1), 32-byte zero-padded reader id, then SyncFiles' CRC32.
constexpr size_t ID_BYTES = 32;
constexpr size_t CLAIM_BYTES = 1 + ID_BYTES + 4;

enum class Claim : uint8_t { None, Found, Unreadable };

bool validId(const char* id) {
  if (strcmp(id, "crossink-simulator") == 0) return true;
  if (strncmp(id, "crossink-", 9) != 0 || strlen(id) != 21) return false;
  for (size_t i = 9; i < 21; ++i)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  return true;
}

// Read once: the efuse MAC does not change. 32 B of DRAM, no heap.
const char* myId() {
  static char id[ID_BYTES];
  static bool known = false;
  if (!known) {
    known = KOReaderSyncClient::readerId(id, sizeof(id)) && validId(id);
    if (!known) LOG_ERR("StatsOrigin", "Cannot read device identity");
  }
  return known ? id : nullptr;
}

bool readOne(const std::string& path, char* out, bool& unreadable) {
  FsFile file;
  if (!Storage.openFileForRead("StatsOrigin", path, file)) {
    unreadable = true;
    return false;
  }
  uint8_t data[CLAIM_BYTES]{};
  const size_t size = file.fileSize();
  const int bytes = file.read(data, sizeof(data));
  const bool closed = file.close();
  if (bytes < 0 || static_cast<size_t>(bytes) != (size < sizeof(data) ? size : sizeof(data)) || !closed) {
    unreadable = true;
    return false;
  }
  // A newer format is uncertain data, even in .tmp: never replace a claim this
  // build does not understand. A short or empty file is a torn first write.
  if (size > sizeof(data) || (size && data[0] != 1)) {
    unreadable = true;
    return false;
  }
  uint32_t checksum = 0;
  for (size_t i = 0; i < 4; ++i) checksum |= uint32_t(data[1 + ID_BYTES + i]) << (8 * i);
  if (size != sizeof(data) || SyncFiles::crc(data, 1 + ID_BYTES, 0) != checksum) return false;
  if (data[ID_BYTES] != 0) {  // the id must end inside its 32 bytes
    unreadable = true;
    return false;
  }
  memcpy(out, data + 1, ID_BYTES);
  if (validId(out)) return true;
  unreadable = true;
  return false;
}

// The claim at `path`, else its .tmp or .bak. Reads only: Library and Home
// load stats per book, and those scans must never write.
Claim readClaim(const std::string& path, char* out) {
  bool exists = false;
  bool unreadable = false;
  for (const char* suffix : {"", ".tmp", ".bak"}) {
    const std::string candidate = path + suffix;
    if (!Storage.exists(candidate.c_str())) continue;
    exists = true;
    if (readOne(candidate, out, unreadable)) return Claim::Found;
  }
  // Only a torn .tmp: the first claim write stopped before it committed, and
  // nothing has relied on it yet. A read error is no such proof.
  const bool interrupted =
      exists && !unreadable && !Storage.exists(path.c_str()) && !Storage.exists((path + ".bak").c_str());
  if (!exists || interrupted) return Claim::None;
  LOG_ERR("StatsOrigin", "Cannot read stats origin: %s", path.c_str());
  return Claim::Unreadable;
}

bool writeClaim(const std::string& path, const char* id) {
  SyncFiles::Writer writer(path.c_str());  // open() clears a torn .tmp
  char stored[ID_BYTES]{};
  snprintf(stored, sizeof(stored), "%s", id);
  if (writer.open() && writer.pod(uint8_t{1}) && writer.write(stored, sizeof(stored)) && writer.commit()) return true;
  LOG_ERR("StatsOrigin", "Cannot write stats origin: %s", path.c_str());
  return false;
}

// Root claims are read once per card session: book loads ask on every Library row.
struct RootClaim {
  bool known = false;
  Claim state = Claim::None;
  char id[ID_BYTES]{};
};
RootClaim roots[2];  // [0] /.crossdink, [1] /.crosspoint

RootClaim& root(const bool twin) {
  RootClaim& r = roots[twin ? 1 : 0];
  if (!r.known) {
    r.state = readClaim(std::string(twin ? TWIN_ROOT : NATIVE_ROOT) + CLAIM, r.id);
    r.known = true;
  }
  return r;
}

// A /.crossdink path that only its /.crosspoint twin holds (HalStorage read-through).
bool isTwinOnly(const std::string& path) {
#ifdef SIMULATOR
  // shortcut: the simulator mocks HalStorage without isTwinOnly, so every file there
  // counts as native; add it to prokrypt/crossdink-simulator to test twins there.
  (void)path;
  return false;
#else
  return Storage.isTwinOnly(path.c_str());
#endif
}

std::string folderOf(const std::string& path) { return path.substr(0, path.rfind('/')); }

// A native folder's /.crosspoint twin would merge into it on rename (HalStorage::rename).
void hideTwinUnlessMine(const std::string& folder, const char* me) {
#ifdef SIMULATOR
  // shortcut: the mocked HalStorage has no twins (see isTwinOnly).
  (void)folder;
  (void)me;
#else
  const std::string twin = TWIN_ROOT + folder.substr(sizeof(NATIVE_ROOT) - 1);
  if (!Storage.exists(twin.c_str())) return;
  const RootClaim& r = root(true);  // a root-level folder: the twin root's record names its owner
  if (r.state == Claim::None || (r.state == Claim::Found && strcmp(r.id, me) == 0)) return;
  LOG_INF("StatsOrigin", "Leaving another reader's %s out of the move", twin.c_str());
  Storage.hideTwin(folder.c_str());
#endif
}

// Moves the files of `from` into `to`, keeping `to`'s copy of a name both hold.
void moveFiles(const std::string& from, const std::string& to) {
  // Names first: renaming while listing would move the directory cursor. One-off (rollback) path.
  std::vector<std::string> names;
  FsFile dir = Storage.open(from.c_str());
  char name[64];
  for (FsFile file = dir ? dir.openNextFile() : FsFile(); file; file = dir.openNextFile()) {
    const bool regular = !file.isDirectory();
    const size_t length = file.getName(name, sizeof(name));
    file.close();
    if (regular && length > 0) names.emplace_back(name);
  }
  dir.close();
  bool kept = false;
  for (const std::string& entry : names) {
    const std::string target = to + "/" + entry;
    if (Storage.exists(target.c_str())) {
      kept = true;
      continue;
    }
    if (!Storage.rename((from + "/" + entry).c_str(), target.c_str()))
      LOG_ERR("StatsOrigin", "Cannot move %s", entry.c_str());
  }
  if (kept) {
    LOG_INF("StatsOrigin", "Kept files %s and %s both hold in %s", from.c_str(), to.c_str(), from.c_str());
  } else {
    Storage.rmdir(from.c_str());
  }
}
}  // namespace

std::string StatsStorage::localPath(const std::string& legacyPath) {
  const char* me = myId();
  if (!me) return {};
  const size_t n = legacyPath.size();
  const size_t stem = n >= 4 && legacyPath.compare(n - 4, 4, ".bin") == 0 ? n - 4 : n;
  return legacyPath.substr(0, stem) + "_" + me + legacyPath.substr(stem);
}

StatsStorage::Owner StatsStorage::legacyOwner(const std::string& legacyPath) {
  const char* me = myId();
  if (!me) return Owner::Unknown;
  // Claims cover files on their own side only: CrossInk's /.crosspoint claim
  // says nothing about what this reader wrote under /.crossdink.
  const bool twin = isTwinOnly(legacyPath);
  std::string folder = folderOf(legacyPath);
  // cppcheck-suppress knownConditionTrueFalse ; constant only in the simulator build
  if (twin) folder.replace(0, sizeof(NATIVE_ROOT) - 1, TWIN_ROOT);
  char owner[ID_BYTES];
  Claim claim = Claim::None;
  // cppcheck-suppress knownConditionTrueFalse ; constant only in the simulator build
  if (folder != (twin ? TWIN_ROOT : NATIVE_ROOT)) claim = readClaim(folder + CLAIM, owner);
  if (claim == Claim::None) {
    const RootClaim& r = root(twin);
    claim = r.state;
    memcpy(owner, r.id, ID_BYTES);
  }
  switch (claim) {
    case Claim::None:
      return Owner::Me;  // nobody claimed them: they count as this reader's
    case Claim::Found:
      return strcmp(owner, me) == 0 ? Owner::Me : Owner::Other;
    case Claim::Unreadable:
      break;
  }
  return Owner::Unknown;
}

bool StatsStorage::beforeFirstSave(const std::string& legacyPath) {
  const char* me = myId();
  if (!me) return false;
  RootClaim& r = root(false);
  if (r.state == Claim::None) {
    if (!writeClaim(std::string(NATIVE_ROOT) + CLAIM, me)) return false;
    r.state = Claim::Found;
    memcpy(r.id, me, ID_BYTES);
  }
  if (legacyPath.empty()) return true;
  const Owner owner = legacyOwner(legacyPath);
  if (owner == Owner::Unknown) {
    LOG_ERR("StatsOrigin", "Not saving over stats with an unreadable origin: %s", legacyPath.c_str());
    return false;
  }
  const std::string folder = folderOf(legacyPath);
  // cppcheck-suppress knownConditionTrueFalse ; constant only in the simulator build
  if (isTwinOnly(legacyPath) || folder == NATIVE_ROOT) return true;  // /.crosspoint is never written
  char existing[ID_BYTES];
  if (readClaim(folder + CLAIM, existing) != Claim::None) return true;
  // No folder claim, so the owner came from the root claim.
  return writeClaim(folder + CLAIM, owner == Owner::Me ? me : r.id);
}

std::string StatsStorage::folderToRead(const std::string& legacyFolder) {
  const std::string local = localPath(legacyFolder);
  if (local.empty() || Storage.exists(local.c_str()) || !Storage.exists(legacyFolder.c_str())) return local;
  return legacyOwner(legacyFolder) == Owner::Me ? legacyFolder : local;
}

std::string StatsStorage::adoptFolder(const std::string& legacyFolder) {
  const std::string local = localPath(legacyFolder);
  if (local.empty() || !Storage.exists(legacyFolder.c_str())) return local;
  switch (legacyOwner(legacyFolder)) {
    case Owner::Unknown:
      LOG_ERR("StatsOrigin", "Not using %s: unreadable origin", legacyFolder.c_str());
      return {};
    case Owner::Other:
      return local;  // another reader's: kept, never uploaded as this reader's
    case Owner::Me:
      break;
  }
  if (!beforeFirstSave("")) return {};  // the root record goes first
  // cppcheck-suppress knownConditionTrueFalse ; constant only in the simulator build
  if (!isTwinOnly(legacyFolder)) hideTwinUnlessMine(legacyFolder, myId());
  if (Storage.exists(local.c_str())) {
    moveFiles(legacyFolder, local);
  } else if (!Storage.rename(legacyFolder.c_str(), local.c_str())) {
    LOG_ERR("StatsOrigin", "Cannot move %s", legacyFolder.c_str());
    return {};
  }
  LOG_INF("StatsOrigin", "Moved %s to %s", legacyFolder.c_str(), local.c_str());
  return local;
}

void StatsStorage::forget() {
  for (RootClaim& r : roots) r.known = false;
}
