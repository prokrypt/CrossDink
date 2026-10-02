#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <FS.h>  // need to be included before SdFat.h for compatibility with FS.h's File class
#include <HalClock.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <PerfLog.h>
#include <SDCardManager.h>
#if FREEINK_CAP_USB_MSC
#include <UsbMassStorage.h>

#include "UsbDriveReadAhead.h"
#endif

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "HalSpiBus.h"

#if CROSSDINK_PERF_LOG
// Debug trace of SD mutations ([SDW]); only successful ones, since a failed
// remove or mkdir probe changed nothing on the card.
#define SDW_LOG(ok, fmt, ...)                   \
  do {                                          \
    if (ok) LOG_DBG("SDW", fmt, ##__VA_ARGS__); \
  } while (0)
#else
#define SDW_LOG(ok, fmt, ...) \
  do {                        \
  } while (0)
#endif

#define SDCard SDCardManager::getInstance()

HalStorage HalStorage::instance;

namespace {
bool isHiddenPath(const char* path) {
  for (const char* p = path; *p; ++p) {
    if (*p == '.' && (p == path || p[-1] == '/')) return true;
  }
  return false;
}

bool extensionIs(const char* ext, const char* expected) {
  for (; *ext && *expected; ++ext, ++expected) {
    if (std::tolower(static_cast<unsigned char>(*ext)) != *expected) return false;
  }
  return *ext == '\0' && *expected == '\0';
}

// Whether a mutation of `path` can change what the Library lists. The Library
// skips dot-prefixed entries (including /.crossdink caches) and lists only
// the extensions in library::fileTypeFor(); a path without an extension is
// treated as a folder. Over-reporting only costs one extra rescan.
bool affectsLibrary(const char* path) {
  if (!path || path[0] == '\0') return false;
  if (isHiddenPath(path)) return false;
  const char* base = std::strrchr(path, '/');
  base = base ? base + 1 : path;
  const char* ext = std::strrchr(base, '.');
  if (!ext) return true;
  return extensionIs(ext, ".epub") || extensionIs(ext, ".xtc") || extensionIs(ext, ".xtch") ||
         extensionIs(ext, ".txt") || extensionIs(ext, ".md");
}

bool isFolderMutation(const char* path) { return path && path[0] != '\0' && !isHiddenPath(path); }
}  // namespace

// Deep sleep keeps RTC memory; power loss and resets do not clear it reliably,
// so the pair is checked and cleared on every mount.
constexpr uint32_t LIBRARY_SCAN_CURRENT_MAGIC = 0x4C494233;  // "LIB3"
RTC_NOINIT_ATTR uint32_t rtcLibraryScanCurrent;
RTC_NOINIT_ATTR uint32_t rtcLibraryScanCurrentCheck;

void HalStorage::noteLibraryScanned(const uint32_t generation) {
  libraryScannedGeneration.store(generation, std::memory_order_release);
  libraryScanned.store(true, std::memory_order_release);
}

bool HalStorage::libraryScanCurrent() const {
  return libraryScanned.load(std::memory_order_acquire) &&
         libraryScannedGeneration.load(std::memory_order_acquire) == libraryContentGeneration();
}

void HalStorage::markLibraryContentChanged(const char* reason) {
  libraryGeneration.fetch_add(1, std::memory_order_acq_rel);
  LOG_DBG("SD", "Library content may have changed: %s", reason ? reason : "?");
}

#if FREEINK_CAP_USB_MSC
class HalStorage::UsbDriveContext {
 public:
  freeink::UsbMassStorage massStorage;
  UsbDriveReadAhead readAhead;
};
#endif

namespace {
constexpr uint16_t kFallbackYear = 2024;
constexpr uint8_t kFallbackMonth = 1;
constexpr uint8_t kFallbackDay = 1;
constexpr uint8_t kFallbackHour = 0;
constexpr uint8_t kFallbackMinute = 0;
HalStorage::UtcOffsetFn utcOffsetQAt = nullptr;

bool isLeapYear(const uint16_t year) { return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0; }

uint8_t daysInMonth(const uint16_t year, const uint8_t month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return days[month - 1];
}

bool isValidFatDateTime(const uint16_t year, const uint8_t month, const uint8_t day, const uint8_t hour,
                        const uint8_t minute) {
  if (year < 1980 || year > 2107 || hour > 23 || minute > 59) return false;
  const uint8_t monthDays = daysInMonth(year, month);
  return monthDays > 0 && day >= 1 && day <= monthDays;
}

void adjustDateByDays(uint16_t& year, uint8_t& month, uint8_t& day, const int dayDelta) {
  if (dayDelta > 0) {
    const uint8_t monthDays = daysInMonth(year, month);
    if (day < monthDays) {
      day++;
      return;
    }
    day = 1;
    if (month < 12) {
      month++;
    } else {
      month = 1;
      year++;
    }
  } else if (dayDelta < 0) {
    if (day > 1) {
      day--;
      return;
    }
    if (month > 1) {
      month--;
    } else {
      month = 12;
      year--;
    }
    day = daysInMonth(year, month);
  }
}

void setPackedFatDateTime(uint16_t* date, uint16_t* time, uint8_t* ms10, const uint16_t year, const uint8_t month,
                          const uint8_t day, const uint8_t hour, const uint8_t minute, const uint8_t second) {
  *date = FS_DATE(year, month, day);
  // FAT stores seconds in 2 s steps; the odd second rides in the 10 ms field.
  *time = FS_TIME(hour, minute, second);
  *ms10 = second & 1 ? 100 : 0;
}

void storageDateTimeCallback(uint16_t* date, uint16_t* time, uint8_t* ms10) {
  uint16_t year = kFallbackYear;
  uint8_t month = kFallbackMonth;
  uint8_t day = kFallbackDay;
  uint8_t hour = kFallbackHour;
  uint8_t minute = kFallbackMinute;
  uint8_t second = 0;

  if (halClock.getDateTime(year, month, day, hour, minute, second) &&
      isValidFatDateTime(year, month, day, hour, minute)) {
    // FAT timestamps are local wall-clock time; the RTC runs in UTC.
    const uint8_t configuredOffsetQ = utcOffsetQAt ? utcOffsetQAt(year, month, day, hour, minute) : 48;
    const uint8_t offsetQ = configuredOffsetQ > 104 ? 104 : configuredOffsetQ;
    const int offsetQuarterHours = static_cast<int>(offsetQ) - 48;
    int localMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) + offsetQuarterHours * 15;
    const int dayDelta = localMinutes < 0 ? -1 : (localMinutes >= 1440 ? 1 : 0);
    localMinutes = ((localMinutes % 1440) + 1440) % 1440;
    adjustDateByDays(year, month, day, dayDelta);
    hour = static_cast<uint8_t>(localMinutes / 60);
    minute = static_cast<uint8_t>(localMinutes % 60);
  }

  if (!isValidFatDateTime(year, month, day, hour, minute) || second > 59) {
    year = kFallbackYear;
    month = kFallbackMonth;
    day = kFallbackDay;
    hour = kFallbackHour;
    minute = kFallbackMinute;
    second = 0;
  }

  setPackedFatDateTime(date, time, ms10, year, month, day, hour, minute, second);
}
}  // namespace

HalStorage::HalStorage()
#if FREEINK_CAP_USB_MSC
    : usbDriveContext(new (std::nothrow) UsbDriveContext())
#endif
{
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

HalStorage::~HalStorage() = default;

// begin() and ready() are only called from setup, no need to acquire mutex for them

bool HalStorage::begin() {
  HalSpiBus::Lock spiLock;
  const bool scanCarriedOver =
      rtcLibraryScanCurrent == LIBRARY_SCAN_CURRENT_MAGIC && rtcLibraryScanCurrentCheck == ~LIBRARY_SCAN_CURRENT_MAGIC;
  rtcLibraryScanCurrent = 0;
  rtcLibraryScanCurrentCheck = 0;
  if (scanCarriedOver) {
    // Waking from deep sleep with no Library change since the last scan. Card
    // edits made elsewhere while asleep need the Library's refresh button.
    noteLibraryScanned(libraryContentGeneration());
  } else {
    // A cold boot or reset may follow edits made on a computer.
    markLibraryContentChanged("mount");
  }
  return SDCard.begin();
}

bool HalStorage::ready() const { return SDCard.ready(); }

bool HalStorage::beginUsbDrive() {
  // The host may change anything while it owns the card.
  markLibraryContentChanged("USB Drive start");
#if FREEINK_CAP_USB_MSC && FREEINK_SD_SDMMC
  if (!usbDriveContext) {
    LOG_ERR("USB", "USB Drive context allocation failed");
    return false;
  }
  auto* const blockDevice = SDCard.detachFilesystemForRawAccess();
  if (!blockDevice) {
    LOG_ERR("USB", "USB Drive requires a mounted SDMMC filesystem");
    return false;
  }
  powerManager.setUsbDriveActive(true);
  // Falls back to plain pass-through reads if its buffers can't be allocated.
  usbDriveContext->readAhead.begin(blockDevice);
  if (!usbDriveContext->massStorage.begin(&usbDriveContext->readAhead)) {
    LOG_ERR("USB", "USB Drive MSC initialization failed");
    usbDriveContext->readAhead.end();
    powerManager.setUsbDriveActive(false);
    if (!SDCard.begin()) {
      LOG_ERR("USB", "Unable to remount SD card after USB Drive startup failure");
    }
    return false;
  }
  return true;
#elif defined(SIMULATOR) && CROSSDINK_APP_CAP_USB_DRIVE
  return true;
#else
  return false;
#endif
}

bool HalStorage::disconnectUsbDriveHost() {
#if FREEINK_CAP_USB_MSC
  return usbDriveContext && usbDriveContext->massStorage.disconnectHost();
#else
  return false;
#endif
}

bool HalStorage::usbDriveHostSuspended() const {
#if FREEINK_CAP_USB_MSC
  return usbDriveContext && usbDriveContext->massStorage.hostSuspended();
#else
  return false;
#endif
}

void HalStorage::endUsbDrive() {
  markLibraryContentChanged("USB Drive end");
#if FREEINK_CAP_USB_MSC
  if (usbDriveContext) {
    usbDriveContext->massStorage.end();
    usbDriveContext->readAhead.end();
  }
  powerManager.setUsbDriveActive(false);
#endif
}

bool HalStorage::usbDriveIo(UsbDriveIo& out) const {
#if FREEINK_CAP_USB_MSC
  if (!usbDriveContext) return false;
  usbDriveContext->readAhead.hostIo(out);
  return true;
#else
  (void)out;
  return false;
#endif
}

UsbDriveState HalStorage::usbDriveState() const {
#if FREEINK_CAP_USB_MSC
  if (!usbDriveContext) return UsbDriveState::Unsupported;
  switch (usbDriveContext->massStorage.state()) {
    case freeink::UsbMassStorageState::WaitingForHost:
      return UsbDriveState::WaitingForHost;
    case freeink::UsbMassStorageState::Connected:
      return UsbDriveState::Connected;
    case freeink::UsbMassStorageState::Accessed:
      return UsbDriveState::Accessed;
    case freeink::UsbMassStorageState::Ejected:
      return UsbDriveState::Ejected;
    case freeink::UsbMassStorageState::Disconnected:
      return UsbDriveState::Disconnected;
    case freeink::UsbMassStorageState::IoError:
      return UsbDriveState::IoError;
    case freeink::UsbMassStorageState::Idle:
      break;
  }
#endif
  return UsbDriveState::Unsupported;
}

// For the rest of the methods, we acquire the mutex to ensure thread safety

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }

 private:
#if !FREEINK_SD_SDMMC
  // SD shares the display's SPI bus. SDMMC cards have their own bus, and
  // taking this lock there only stalls reads behind EPD refreshes.
  HalSpiBus::Lock spiLock;
#endif
};

#define HAL_STORAGE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;               \
  return SDCard.method(__VA_ARGS__);

// CrossDink keeps its data in /.crossdink and reads CrossInk's /.crosspoint in
// its place: a /.crossdink path that does not exist is read from its
// /.crosspoint twin, and folder listings show both. Writes always land in
// /.crossdink; an in-place update (read-write or append open) of a file that
// only has a twin copies that one file first. A removed path whose twin
// exists is listed in /.crossdink/.deleted so the twin stops showing through.
// /.crosspoint is never written, so CrossInk and older builds keep their data.
// EPUB caches are keyed by content, so lib/Epub copies each book's on its
// first open instead.
namespace {
constexpr char kDataRoot[] = "/.crossdink";
constexpr size_t kDataRootLen = sizeof(kDataRoot) - 1;
constexpr char kLegacyRoot[] = "/.crosspoint";
constexpr char kDeletedList[] = "/.crossdink/.deleted";
enum class LegacyRoot : uint8_t { Unknown, Absent, Present };
LegacyRoot legacyRoot = LegacyRoot::Unknown;
std::vector<uint32_t> deletedPaths;  // hashes of the paths in .deleted
// Only touched under the storage lock; kept off the caller's task stack.
char twinPath[256];
uint8_t copyBuf[512];

uint32_t hashPath(const char* s, const size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) h = (h ^ static_cast<uint8_t>(s[i])) * 16777619u;
  return h;
}

bool isTempName(const char* name) {
  const size_t len = strlen(name);
  for (const char* suffix : {".tmp", ".part", ".stage", ".new"}) {
    const size_t n = strlen(suffix);
    if (len >= n && strcmp(name + len - n, suffix) == 0) return true;
  }
  return strcmp(name, "ota-update.bin") == 0;
}

// settings.json(.bak) never reads through: /.crosspoint/settings.json belongs to
// CrossPoint/CrossInk, and CrossPointSettings imports it explicitly.
bool inDataRoot(const char* path) {
  return path && strncmp(path, kDataRoot, kDataRootLen) == 0 &&
         (path[kDataRootLen] == '/' || path[kDataRootLen] == '\0') && strncmp(path + kDataRootLen, "/epub_", 6) != 0 &&
         strncmp(path + kDataRootLen, "/settings.json", 14) != 0;
}

// Caller holds the storage lock. True when `path` or a folder above it was removed.
bool isDeletedLocked(const char* path) {
  if (deletedPaths.empty()) return false;
  for (size_t i = kDataRootLen + 1;; i++) {
    if (path[i] == '/' || path[i] == '\0') {
      if (std::find(deletedPaths.begin(), deletedPaths.end(), hashPath(path, i)) != deletedPaths.end()) return true;
    }
    if (path[i] == '\0') return false;
  }
}

// Caller holds the storage lock. The /.crosspoint twin of `path` if it exists
// and shows through (in twinPath, valid until the lock is released), else null.
const char* legacyTwinLocked(const char* path) {
  if (!inDataRoot(path)) return nullptr;
  if (legacyRoot == LegacyRoot::Unknown) {
    if (!SDCard.ready()) return nullptr;
    legacyRoot = SDCard.exists(kLegacyRoot) ? LegacyRoot::Present : LegacyRoot::Absent;
    FsFile list = legacyRoot == LegacyRoot::Present ? SDCard.open(kDeletedList, O_RDONLY) : FsFile();
    size_t n = 0;
    for (int c; list && (c = list.read()) >= 0;) {
      if (c != '\n') {
        if (n < sizeof(twinPath)) twinPath[n++] = static_cast<char>(c);
        continue;
      }
      deletedPaths.push_back(hashPath(twinPath, n));
      n = 0;
    }
    list.close();
  }
  if (legacyRoot == LegacyRoot::Absent || isDeletedLocked(path)) return nullptr;
  if (snprintf(twinPath, sizeof(twinPath), "%s%s", kLegacyRoot, path + kDataRootLen) >=
      static_cast<int>(sizeof(twinPath)))
    return nullptr;
  return SDCard.exists(twinPath) ? twinPath : nullptr;
}

// Caller holds the storage lock. Writes create missing /.crossdink folders,
// since a folder may so far exist only in /.crosspoint.
void ensureParentLocked(const char* path) {
  if (!inDataRoot(path)) return;
  const char* slash = strrchr(path, '/');
  if (!slash || slash == path) return;
  const std::string parent(path, slash - path);
  if (!SDCard.exists(parent.c_str())) SDCard.mkdir(parent.c_str(), true);
}

// Caller holds the storage lock. Hides the twin of a removed path.
void markDeletedLocked(const char* path) {
  size_t len = strlen(path);
  while (len > kDataRootLen && path[len - 1] == '/') len--;
  deletedPaths.push_back(hashPath(path, len));
  SDCard.mkdir(kDataRoot, true);
  FsFile list = SDCard.open(kDeletedList, O_WRONLY | O_CREAT | O_APPEND);
  if (!list || list.write(path, len) != len || list.write('\n') != 1) {
    LOG_ERR("SD", "Cannot record %s in %s", path, kDeletedList);
  }
  list.close();
}

// Caller holds the storage lock. Copies the /.crosspoint file or folder `from`
// to `to`, keeping whatever `to` already has.
bool copyTwinLocked(const std::string& from, const std::string& to) {
  FsFile in = SDCard.open(from.c_str(), O_RDONLY);
  if (!in) return false;
  if (in.isDirectory()) {
    SDCard.mkdir(to.c_str(), true);
    bool ok = true;
    char name[64];
    for (FsFile entry = in.openNextFile(); entry; entry = in.openNextFile()) {
      const size_t nameLen = entry.getName(name, sizeof(name));
      entry.close();
      if (nameLen == 0 || nameLen >= sizeof(name) - 1 || isTempName(name)) continue;
      ok = copyTwinLocked(from + "/" + name, to + "/" + name) && ok;
    }
    in.close();
    return ok;
  }
  if (SDCard.exists(to.c_str())) {
    in.close();
    return true;
  }
  const std::string part = to + ".part";
  FsFile out = SDCard.open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  bool ok = static_cast<bool>(out);
  for (int n; ok && (n = in.read(copyBuf, sizeof(copyBuf))) != 0;) {
    ok = n > 0 && out.write(copyBuf, n) == static_cast<size_t>(n);
  }
  in.close();
  ok = ok && out.sync();
  out.close();
  if (ok && SDCard.rename(part.c_str(), to.c_str())) return true;
  SDCard.remove(part.c_str());
  LOG_ERR("SD", "Copy failed: %s -> %s", from.c_str(), to.c_str());
  return false;
}
}  // namespace

void HalStorage::shutdown() {
  const bool current = libraryScanCurrent();
  rtcLibraryScanCurrent = current ? LIBRARY_SCAN_CURRENT_MAGIC : 0;
  rtcLibraryScanCurrentCheck = current ? ~LIBRARY_SCAN_CURRENT_MAGIC : 0;
  StorageLock lock;
  SDCard.shutdown();
}

uint64_t HalStorage::totalBytes() const { return SDCard.sdTotalBytes(); }

uint64_t HalStorage::usedBytes() { HAL_STORAGE_WRAPPED_CALL(sdUsedBytes); }

// The path to read for `path`: itself, or its /.crosspoint twin when only that
// exists. Caller holds the storage lock; the result is valid until it is released.
static const char* readablePathLocked(const char* path) {
  if (!inDataRoot(path) || SDCard.exists(path)) return path;
  const char* twin = legacyTwinLocked(path);
  return twin ? twin : path;
}

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  HAL_STORAGE_WRAPPED_CALL(listFiles, readablePathLocked(path), maxFiles);
}

String HalStorage::readFile(const char* path) { HAL_STORAGE_WRAPPED_CALL(readFile, readablePathLocked(path)); }

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HAL_STORAGE_WRAPPED_CALL(readFileToStream, readablePathLocked(path), out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  HAL_STORAGE_WRAPPED_CALL(readFileToBuffer, readablePathLocked(path), buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  if (affectsLibrary(path)) markLibraryContentChanged(path);
  bool ok;
  {
    StorageLock lock;
    ensureParentLocked(path);
    ok = SDCard.writeFile(path, content);
  }
  SDW_LOG(ok, "write - %s %u B", path, static_cast<unsigned>(content.length()));
  return ok;
}

bool HalStorage::ensureDirectoryExists(const char* path) {
#if CROSSDINK_PERF_LOG
  bool ok;
  bool existed;
  {
    StorageLock lock;
    existed = SDCard.exists(path);
    ok = SDCard.ensureDirectoryExists(path);
  }
  SDW_LOG(ok && !existed, "mkdir %s", path);
  return ok;
#else
  HAL_STORAGE_WRAPPED_CALL(ensureDirectoryExists, path);
#endif
}

void HalStorage::installDateTimeCallback(const UtcOffsetFn utcOffsetQuarterHoursAt) {
  if (!halClock.isAvailable()) return;
  utcOffsetQAt = utcOffsetQuarterHoursAt;
  FsDateTime::setCallback(storageDateTimeCallback);
  LOG_INF("SD", "Installed RTC-backed SD timestamp callback");
}

// A /.crossdink folder handle that also lists its /.crosspoint twin.
struct HalFile::LegacyMerge {
  FsFile dir;            // the /.crosspoint folder, listed after the handle's own entries
  std::string dataDir;   // the /.crossdink path that was opened
  bool handleIsLegacy;   // no /.crossdink folder yet: the handle is the twin itself
  bool ownDone = false;  // the handle's own entries are used up
  ~LegacyMerge() { dir.close(); }

  // Caller holds the storage lock. A twin entry stays hidden when it was
  // removed, is an EPUB cache or temp file, or /.crossdink lists it already.
  bool hides(FsFile& entry) const {
    char name[64];
    const size_t nameLen = entry.getName(name, sizeof(name));
    if (nameLen == 0 || nameLen >= sizeof(name) - 1 || isTempName(name) ||
        (entry.isDirectory() && strncmp(name, "epub_", 5) == 0))
      return true;
    const std::string path = dataDir + "/" + name;
    return isDeletedLocked(path.c_str()) || (!handleIsLegacy && SDCard.exists(path.c_str()));
  }
};

class HalFile::Impl {
 public:
  Impl(FsFile&& fsFile) : file(std::move(fsFile)) {}
  FsFile file;
  std::unique_ptr<LegacyMerge> merge;
#if CROSSDINK_PERF_LOG
  // [SDW] bookkeeping for handles opened to write: one line at close with the
  // path tail, bytes written and time spent in write().
  void tagWrite(const char* module, const char* path) {
    writeModule = module ? module : "-";
    const size_t len = path ? strlen(path) : 0;
    const char* tail = len >= sizeof(writePath) ? path + len - (sizeof(writePath) - 1) : (path ? path : "");
    snprintf(writePath, sizeof(writePath), "%s", tail);
  }
  const char* writeModule = nullptr;
  char writePath[56] = "";
  uint32_t writeBytes = 0;
  uint32_t writeUs = 0;
#endif
};

void HalFile::ImplDeleter::operator()(Impl* const impl) const {
  if (!impl) return;
  impl->~Impl();
  std::free(impl);
}

void* HalFile::allocateImplStorage() {
  // The ESP32 build disables exceptions, so `new (std::nothrow)` can still
  // terminate through libstdc++ when the allocation cannot be satisfied.
  // malloc gives this storage wrapper the recoverable failure contract callers
  // expect while preserving FsFile's value semantics.
  return std::malloc(sizeof(Impl));
}

HalFile::HalFile() = default;

HalFile::HalFile(ImplPtr impl) : impl(std::move(impl)) {}

HalFile::~HalFile() { close(); }

HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&& other) {
  if (this == &other) return *this;
  close();
  impl = std::move(other.impl);
  allocationFailed_ = other.allocationFailed_;
  iterationFailed_ = other.iterationFailed_;
  other.allocationFailed_ = false;
  other.iterationFailed_ = false;
  return *this;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  const bool writing = (oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0;
  if (writing && affectsLibrary(path)) {
    markLibraryContentChanged(path);
  }
  FsFile fsFile;
  std::unique_ptr<HalFile::LegacyMerge> merge;
  {
    StorageLock lock;  // ensure thread safety for the duration of this function
    if (writing && inDataRoot(path)) {
      // An in-place update starts from the /.crosspoint copy when that is all there is.
      if ((oflag & O_TRUNC) == 0 && !SDCard.exists(path)) {
        const char* twin = legacyTwinLocked(path);
        if (twin) copyTwinLocked(twin, path);
      }
      if (oflag & O_CREAT) ensureParentLocked(path);
    }
    fsFile = SDCard.open(path, oflag);
    if (!writing && inDataRoot(path)) {
      // A folder lists its /.crosspoint twin's entries too; a missing path
      // reads the twin.
      const bool found = static_cast<bool>(fsFile);
      if (!found || fsFile.isDirectory()) {
        const char* twin = legacyTwinLocked(path);
        FsFile twinFile = twin ? SDCard.open(twin, O_RDONLY) : FsFile();
        if (!found) {
          fsFile = std::move(twinFile);
          if (fsFile && fsFile.isDirectory())
            merge.reset(new (std::nothrow) HalFile::LegacyMerge{FsFile(), path, true});
        } else if (twinFile && twinFile.isDirectory()) {
          merge.reset(new (std::nothrow) HalFile::LegacyMerge{std::move(twinFile), path, false});
        } else {
          twinFile.close();
        }
      }
    }
  }
  PerfLog::noteSdOpen(static_cast<bool>(fsFile));
  if (!fsFile) {
    return HalFile();
  }
  void* const storage = HalFile::allocateImplStorage();
  HalFile::ImplPtr impl(storage ? ::new (storage) HalFile::Impl(std::move(fsFile)) : nullptr);
  if (!impl) {
    LOG_ERR("SD", "OOM: HalFile wrapper for %s (%u free, %u max alloc)", path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    StorageLock lock;
    fsFile.close();
    HalFile failed;
    failed.allocationFailed_ = true;
    return failed;
  }
#if CROSSDINK_PERF_LOG
  if ((oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND)) != 0) impl->tagWrite(nullptr, path);
#endif
  impl->merge = std::move(merge);
  return HalFile(std::move(impl));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
#if CROSSDINK_PERF_LOG
  bool ok;
  bool existed;
  {
    StorageLock lock;
    // Probe so the common mkdir of an existing directory is not logged.
    existed = SDCard.exists(path);
    ok = SDCard.mkdir(path, pFlag);
  }
  SDW_LOG(ok && !existed, "mkdir %s", path);
  return ok;
#else
  HAL_STORAGE_WRAPPED_CALL(mkdir, path, pFlag);
#endif
}

bool HalStorage::exists(const char* path) {
  StorageLock lock;
  return SDCard.exists(path) || legacyTwinLocked(path) != nullptr;
}

bool HalStorage::remove(const char* path) {
  if (affectsLibrary(path)) markLibraryContentChanged(path);
  bool ok;
  {
    StorageLock lock;
    ok = SDCard.remove(path);
    if (legacyTwinLocked(path)) {
      markDeletedLocked(path);
      ok = true;
    }
  }
  SDW_LOG(ok, "remove %s", path);
  return ok;
}
bool HalStorage::rename(const char* oldPath, const char* newPath) {
  if (affectsLibrary(oldPath) || affectsLibrary(newPath)) markLibraryContentChanged(newPath);
  bool ok;
  {
    StorageLock lock;
    ensureParentLocked(newPath);
    ok = SDCard.rename(oldPath, newPath);
    // What only /.crosspoint holds under oldPath moves along as a copy.
    if (const char* twin = legacyTwinLocked(oldPath)) {
      const std::string from(twin);
      if (copyTwinLocked(from, newPath)) {
        markDeletedLocked(oldPath);
        ok = true;
      } else {
        ok = false;
      }
    }
  }
  SDW_LOG(ok, "rename %s -> %s", oldPath, newPath);
  return ok;
}

bool HalStorage::rmdir(const char* path) {
  if (isFolderMutation(path)) markLibraryContentChanged(path);
  bool ok;
  {
    StorageLock lock;
    ok = SDCard.rmdir(path);
    if (legacyTwinLocked(path)) {
      markDeletedLocked(path);
      ok = true;
    }
  }
  SDW_LOG(ok, "rmdir %s", path);
  return ok;
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  return openFileForRead(moduleName, path, file, /*quietMiss=*/false);
}

bool HalStorage::openFileForReadIfPresent(const char* moduleName, const char* path, HalFile& file) {
  return openFileForRead(moduleName, path, file, /*quietMiss=*/true);
}

bool HalStorage::openFileForReadIfPresent(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file, /*quietMiss=*/true);
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file, const bool quietMiss) {
  file.close();
  FsFile fsFile;
  bool ok = false;
  {
    StorageLock lock;  // ensure thread safety for the duration of this function
    if (inDataRoot(path)) {
      fsFile = SDCard.open(path, O_RDONLY);
      if (!fsFile) {
        const char* twin = legacyTwinLocked(path);
        if (twin) fsFile = SDCard.open(twin, O_RDONLY);
      }
      ok = static_cast<bool>(fsFile);
      if (!ok && !quietMiss) LOG_DBG("SD", "%s: open for read failed: %s", moduleName, path);
    } else if (quietMiss) {
      // One open, no failure print: a missing file is an expected answer here.
      fsFile = SDCard.open(path, O_RDONLY);
      ok = static_cast<bool>(fsFile);
    } else {
      ok = SDCard.openFileForRead(moduleName, path, fsFile);
    }
  }
  PerfLog::noteSdOpen(ok);
  if (!ok) {
    return false;
  }
  void* const storage = HalFile::allocateImplStorage();
  HalFile::ImplPtr impl(storage ? ::new (storage) HalFile::Impl(std::move(fsFile)) : nullptr);
  if (!impl) {
    LOG_ERR(moduleName, "OOM: HalFile read wrapper for %s (%u free, %u max alloc)", path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    StorageLock lock;
    fsFile.close();
    return false;
  }
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  if (affectsLibrary(path)) markLibraryContentChanged(path);
  file.close();
  FsFile fsFile;
  bool ok = false;
  {
    StorageLock lock;  // ensure thread safety for the duration of this function
    ensureParentLocked(path);
    ok = SDCard.openFileForWrite(moduleName, path, fsFile);
  }
  if (!ok) {
    return false;
  }
  void* const storage = HalFile::allocateImplStorage();
  HalFile::ImplPtr impl(storage ? ::new (storage) HalFile::Impl(std::move(fsFile)) : nullptr);
  if (!impl) {
    LOG_ERR(moduleName, "OOM: HalFile write wrapper for %s (%u free, %u max alloc)", path, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    StorageLock lock;
    fsFile.close();
    return false;
  }
#if CROSSDINK_PERF_LOG
  impl->tagWrite(moduleName, path);
#endif
  file = HalFile(std::move(impl));
  return true;
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
  if (!path || path[0] == '\0') {
    return false;
  }

  HalFile dir = open(path);
  if (!dir || !dir.isDirectory()) {
    dir.close();
    return false;
  }

  char name[128];
  for (HalFile entry = dir.openNextFile(); entry; entry = dir.openNextFile()) {
    const bool isDirectory = entry.isDirectory();
    const size_t nameLen = entry.getName(name, sizeof(name));

    // SdFat cannot reopen or delete this path while its directory entry is open.
    entry.close();
    if (nameLen == 0) {
      dir.close();
      return false;
    }

    std::string entryPath(path);
    if (entryPath.back() != '/') {
      entryPath += '/';
    }
    entryPath += name;

    const bool removed = isDirectory ? removeDir(entryPath.c_str()) : remove(entryPath.c_str());
    if (!removed) {
      dir.close();
      return false;
    }
  }

  dir.close();
  return rmdir(path);
}

// HalFile implementation
// Allow doing file operations while ensuring thread safety via HalStorage's mutex.
// Please keep the list below in sync with the HalFile.h header

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

#define HAL_FILE_FORWARD_CALL(method, ...) \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__);

void HalFile::flush() { HAL_FILE_WRAPPED_CALL(flush, ); }
size_t HalFile::getName(char* name, size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { HAL_FILE_FORWARD_CALL(size, ); }              // already thread-safe, no need to wrap
size_t HalFile::fileSize() { HAL_FILE_FORWARD_CALL(fileSize, ); }      // already thread-safe, no need to wrap
uint64_t HalFile::fileSize64() { HAL_FILE_FORWARD_CALL(fileSize, ); }  // already thread-safe, no need to wrap
uint32_t HalFile::creationTime() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl->file.getCreateDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
uint32_t HalFile::modificationTime() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  uint16_t date = 0;
  uint16_t time = 0;
  if (!impl->file.getModifyDateTime(&date, &time) || date == 0) return 0;
  return (static_cast<uint32_t>(date) << 16) | time;
}
bool HalFile::seek(size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seekCur(int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* buf, size_t count) {
#if CROSSDINK_PERF_LOG
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  const uint32_t startUs = micros();
  const int n = impl->file.read(buf, count);
  PerfLog::noteSdRead(n > 0 ? static_cast<uint32_t>(n) : 0, micros() - startUs);
  return n;
#else
  HAL_FILE_WRAPPED_CALL(read, buf, count);
#endif
}
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }
size_t HalFile::write(const void* buf, size_t count) {
#if CROSSDINK_PERF_LOG
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  const uint32_t startUs = micros();
  const size_t n = impl->file.write(buf, count);
  const uint32_t us = micros() - startUs;
  PerfLog::noteSdWrite(static_cast<uint32_t>(n), us);
  impl->writeBytes += n;
  impl->writeUs += us;
  return n;
#else
  HAL_FILE_WRAPPED_CALL(write, buf, count);
#endif
}
size_t HalFile::write(uint8_t b) {
#if CROSSDINK_PERF_LOG
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  const size_t n = impl->file.write(b);
  impl->writeBytes += n;
  return n;
#else
  HAL_FILE_WRAPPED_CALL(write, b);
#endif
}
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }
bool HalFile::rename(const char* newPath) {
  // The old name is unknown here; treat any handle rename as a content change.
  Storage.markLibraryContentChanged(newPath);
#if CROSSDINK_PERF_LOG
  bool ok;
  {
    HalStorage::StorageLock lock;
    assert(impl != nullptr);
    ok = impl->file.rename(newPath);
  }
  SDW_LOG(ok, "rename (handle) -> %s", newPath);
  if (ok && impl->writeModule) impl->tagWrite(impl->writeModule, newPath);
  return ok;
#else
  HAL_FILE_WRAPPED_CALL(rename, newPath);
#endif
}
bool HalFile::isDirectory() const { HAL_FILE_FORWARD_CALL(isDirectory, ); }  // already thread-safe, no need to wrap
void HalFile::rewindDirectory() {
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  impl->file.rewindDirectory();
  if (impl->merge) {
    impl->merge->dir.rewindDirectory();
    impl->merge->ownDone = false;
  }
  allocationFailed_ = false;
  // SdFat's read-error bits are sticky for the lifetime of the handle and
  // FsFile does not expose clearError(). Reopen the directory to retry after
  // an iteration failure rather than making rewind appear to clear it.
}
bool HalFile::close() {
  if (!impl) return true;
#if CROSSDINK_PERF_LOG
  const char* module = impl->writeModule;
  char path[sizeof(impl->writePath)];
  memcpy(path, impl->writePath, sizeof(path));
  const uint32_t bytes = impl->writeBytes;
  const uint32_t us = impl->writeUs;
#endif
  bool ok;
  {
    HalStorage::StorageLock lock;
    ok = impl->file.close();
    impl.reset();
  }
  allocationFailed_ = false;
  iterationFailed_ = false;
#if CROSSDINK_PERF_LOG
  // Opened to write: a truncating open is a mutation even with no bytes.
  SDW_LOG(path[0] != '\0', "write %s %s %lu B %lu ms", module ? module : "-", path, static_cast<unsigned long>(bytes),
          static_cast<unsigned long>(us / 1000));
#endif
  return ok;
}
HalFile HalFile::openNextFile() {
  allocationFailed_ = false;
  iterationFailed_ = false;
  HalStorage::StorageLock lock;
  assert(impl != nullptr);
  LegacyMerge* const merge = impl->merge.get();
  FsFile fsFile;
  if (!merge || !merge->ownDone) fsFile = impl->file.openNextFile();
  if (!fsFile && impl->file.getError() != 0) {
    iterationFailed_ = true;
    LOG_ERR("SD", "Directory iteration failed (SdFat error 0x%02x)", impl->file.getError());
    return HalFile();
  }
  if (merge && (merge->handleIsLegacy || !fsFile)) {
    FsFile& twin = merge->handleIsLegacy ? impl->file : merge->dir;
    if (!merge->handleIsLegacy) {
      merge->ownDone = true;
      fsFile = twin.openNextFile();
    }
    while (fsFile && merge->hides(fsFile)) {
      fsFile.close();
      fsFile = twin.openNextFile();
    }
  }
  if (!fsFile) return HalFile();
  void* const storage = allocateImplStorage();
  ImplPtr childImpl(storage ? ::new (storage) Impl(std::move(fsFile)) : nullptr);
  if (!childImpl) {
    allocationFailed_ = true;
    LOG_ERR("SD", "OOM: HalFile directory entry wrapper (%u free, %u max alloc)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    fsFile.close();
    return HalFile();
  }
  return HalFile(std::move(childImpl));
}
bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }  // already thread-safe, no need to wrap
HalFile::operator bool() const { return isOpen(); }
