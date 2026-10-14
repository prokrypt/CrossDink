#include "BugReport.h"

#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Knobs.h>
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PsramLog.h>
#include <WiFi.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "KOReaderCredentialStore.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/util/BugReportActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/Toast.h"
#include "network/SerialRemote.h"
#include "util/BootReason.h"
#include "util/BugReportSanitize.h"
#include "util/BuildInfo.h"
#include "util/ScreenshotUtil.h"
#include "util/StoredZip.h"
#include "util/WorkerTask.h"

#ifndef SIMULATOR
#include <esp_http_client.h>
#endif

extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;
extern SdCardFontSystem sdFontSystem;

namespace BugReport {
namespace {

constexpr size_t LOG_BYTES = 64 * 1024;  // tail of the PSRAM ring (user pick)
constexpr size_t MAX_LINE_BYTES = 512;
constexpr const char* BATTERY_LOG = "/debug/logs/battery.csv";  // BatteryLog::LOG_PATH, Goodies builds only

// Print::printf is not in every build's Print, so format here.
void put(HalFile& f, const char* fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  if (n > 0) f.write(line, std::min<size_t>(static_cast<size_t>(n), sizeof(line) - 1));
}

// Working folder for one report. Hidden, so the Library never indexes its .txt
// files; save() bundles it into <id>.zip and deletes it.
void workDir(char* out, const size_t cap, const char* id) { snprintf(out, cap, "%s/.%s", DIR, id); }

void path(char* out, const size_t cap, const char* id, const char* name) {
  snprintf(out, cap, "%s/.%s/%s", DIR, id, name);
}

void zipPath(char* out, const size_t cap, const char* id, const bool sent) {
  snprintf(out, cap, "%s/%s%s", DIR, id, sent ? ".sent.zip" : ".zip");
}

constexpr const char* BUNDLE_FILES[] = {"report.txt", "log.txt", "screen.png"};

// Stored (uncompressed) zip: the device has
// no deflate. Two reads per file: CRC first, since the local header needs it.
bool bundle(const char* id) {
  char zip[64];
  zipPath(zip, sizeof(zip), id, false);
  auto chunk = makeUniqueNoThrow<uint8_t[]>(1024);
  if (!chunk) {
    LOG_ERR("BUG", "zip: no memory");
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("BUG", zip, out)) return false;
  StoredZip::Entry entries[3] = {};
  uint16_t count = 0;
  uint32_t offset = 0;
  uint8_t header[StoredZip::CENTRAL_HEADER + 16];
  bool ok = true;
  for (const char* name : BUNDLE_FILES) {
    char file[64];
    path(file, sizeof(file), id, name);
    HalFile in;
    if (!Storage.openFileForReadIfPresent("BUG", file, in)) continue;
    StoredZip::Entry& e = entries[count];
    e = {name, 0, static_cast<uint32_t>(in.fileSize()), offset};
    int r;
    while ((r = in.read(chunk.get(), 1024)) > 0) e.crc = StoredZip::crc32(e.crc, chunk.get(), static_cast<size_t>(r));
    in.seek(0);
    const size_t n = StoredZip::localHeader(e, header);
    ok = out.write(header, n) == n;
    offset += n;
    while (ok && (r = in.read(chunk.get(), 1024)) > 0) {
      ok = out.write(chunk.get(), static_cast<size_t>(r)) == static_cast<size_t>(r);
      offset += static_cast<uint32_t>(r);
    }
    in.close();
    if (!ok) break;
    ++count;
  }
  const uint32_t centralOffset = offset;
  for (uint16_t i = 0; ok && i < count; ++i) {
    const size_t n = StoredZip::centralHeader(entries[i], header);
    ok = out.write(header, n) == n;
    offset += n;
  }
  if (ok) {
    const size_t n = StoredZip::endRecord(count, offset - centralOffset, centralOffset, header);
    ok = out.write(header, n) == n;
  }
  ok = out.close() && ok;
  if (!ok) {
    LOG_ERR("BUG", "zip: write failed %s", zip);
    Storage.remove(zip);
    return false;
  }
  char dir[48];
  workDir(dir, sizeof(dir), id);
  Storage.removeDir(dir);
  return true;
}

// "YYYYMMDD-HHMMSS-xxxx" from the RTC, or "m<millis>-xxxx" without a clock.
void makeId(char (&id)[24]) {
  uint16_t year;
  uint8_t month, day, hour, minute, second;
  const unsigned tag = static_cast<unsigned>(micros() & 0xFFFF);
  if (halClock.isAvailable() && halClock.getDateTime(year, month, day, hour, minute, second)) {
    snprintf(id, sizeof(id), "%04u%02u%02u-%02u%02u%02u-%04x", year, month, day, hour, minute, second, tag);
  } else {
    snprintf(id, sizeof(id), "m%lu-%04x", static_cast<unsigned long>(millis()), tag);
  }
}

// Secrets the sanitizer replaces, copied once from the loaded stores and freed
// with the report (well under 1 KB).
struct SecretTable {
  std::vector<std::string> values;
  std::vector<BugReportSanitize::Secret> list;
  char wifiTags[8][10] = {};

  void add(const std::string& value, const char* tag) {
    if (value.size() >= BugReportSanitize::MIN_SECRET_LEN) {
      values.push_back(value);
      list.push_back({nullptr, tag});
    }
  }
  void build() {
    values.reserve(32);
    list.reserve(32);
    for (size_t i = 0; i < WIFI_STORE.getCredentialCount() && i < 8; ++i) {
      if (const auto cred = WIFI_STORE.getCredentialAt(i)) {
        snprintf(wifiTags[i], sizeof(wifiTags[i]), "[wifi%u]", static_cast<unsigned>(i + 1));
        add(cred->ssid, wifiTags[i]);
        add(cred->password, "[pw]");
      }
    }
    for (const OpdsServer& server : OPDS_STORE.getServers()) {
      add(server.username, "[opds-user]");
      add(server.password, "[pw]");
    }
    add(KOREADER_STORE.getUsername(), "[kosync-user]");
    add(KOREADER_STORE.getPassword(), "[pw]");
    add(KOREADER_STORE.getMd5Password(), "[pw]");
#if CROSSDINK_SERIAL_REMOTE
    char token[SerialRemote::TOKEN_BUF] = {};
    if (SerialRemote::readToken(token) > 0) add(token, "[token]");
    memset(token, 0, sizeof(token));
#endif
    // Longest first, so a password that contains an SSID is replaced whole.
    std::vector<size_t> order(values.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [this](size_t a, size_t b) { return values[a].size() > values[b].size(); });
    std::vector<BugReportSanitize::Secret> sorted;
    sorted.reserve(order.size());
    for (const size_t i : order) sorted.push_back({values[i].c_str(), list[i].tag});
    list.swap(sorted);
  }
};

// Splits text into lines and writes each one sanitized.
class LineSanitizer {
 public:
  LineSanitizer(HalFile& out, const SecretTable& secrets, char* line, char* clean)
      : out(out), secrets(secrets), line(line), clean(clean) {}

  void feed(const char* data, const size_t len) {
    for (size_t i = 0; i < len; ++i) {
      const char c = data[i];
      if (c == '\n') {
        flush();
        continue;
      }
      if (used < MAX_LINE_BYTES - 1) line[used++] = c;  // overlong lines are cut
    }
  }
  void flush() {
    const size_t n =
        BugReportSanitize::sanitizeLine(line, used, clean, MAX_LINE_BYTES, secrets.list.data(), secrets.list.size());
    out.write(clean, n);
    out.write("\n", 1);
    used = 0;
  }
  void finish() {
    if (used) flush();
  }

 private:
  HalFile& out;
  const SecretTable& secrets;
  char* line;
  char* clean;
  size_t used = 0;
};

bool writeLog(const Context& ctx, const SecretTable& secrets) {
  char file[64];
  path(file, sizeof(file), ctx.id, "log.txt");
  // Chunk + raw line + sanitized line: heap, not the loop task's stack.
  auto buf = makeUniqueNoThrow<char[]>(MAX_LINE_BYTES * 3);
  if (!buf) {
    LOG_ERR("BUG", "log: no memory for buffers");
    return false;
  }
  HalFile out;
  if (!Storage.openFileForWrite("BUG", file, out)) return false;
  char* chunk = buf.get();
  LineSanitizer lines(out, secrets, buf.get() + MAX_LINE_BYTES, buf.get() + MAX_LINE_BYTES * 2);

#if CROSSDINK_HAS_PSRAM_LOG_RING
  const uint32_t end = PsramLog::end();
  uint32_t cursor = std::max<uint32_t>(PsramLog::oldest(), end > LOG_BYTES ? end - LOG_BYTES : 0);
  bool first = true;
  size_t n;
  while (cursor < end && (n = PsramLog::read(cursor, chunk, std::min<size_t>(MAX_LINE_BYTES, end - cursor))) > 0) {
    size_t skip = 0;
    if (first) {  // drop the partial first line
      while (skip < n && chunk[skip] != '\n') ++skip;
      if (skip < n) ++skip;
      first = false;
    }
    lines.feed(chunk + skip, n - skip);
  }
#else
  const std::string rtc = getLastLogs();
  lines.feed(rtc.data(), rtc.size());
#endif
  lines.finish();

  HalFile crash;
  if (Storage.openFileForReadIfPresent("BUG", "/debug/crash_report.txt", crash)) {
    static constexpr char header[] = "\n--- /debug/crash_report.txt ---\n";
    out.write(header, sizeof(header) - 1);
    int r;
    while ((r = crash.read(chunk, MAX_LINE_BYTES)) > 0) lines.feed(chunk, static_cast<size_t>(r));
    lines.finish();
    crash.close();
  }
  const bool ok = out.close();
  if (!ok) LOG_ERR("BUG", "log: write failed %s", file);
  return ok;
}

// Last full row of the battery log, or "" (Goodies builds only write it).
void lastBatteryRow(char* out, const size_t cap) {
  out[0] = '\0';
  HalFile f;
  if (!Storage.openFileForReadIfPresent("BUG", BATTERY_LOG, f)) return;
  char tail[256];
  const size_t size = f.fileSize();
  const size_t start = size > sizeof(tail) - 1 ? size - (sizeof(tail) - 1) : 0;
  f.seek(start);
  const int n = f.read(tail, sizeof(tail) - 1);
  f.close();
  if (n <= 0) return;
  size_t len = static_cast<size_t>(n);
  while (len && (tail[len - 1] == '\n' || tail[len - 1] == '\r')) --len;
  tail[len] = '\0';
  const char* row = strrchr(tail, '\n');
  snprintf(out, cap, "%s", row ? row + 1 : tail);
}

// ~1.3 KB of stack at its deepest (description, battery row, put()): once per
// report on the loop task, which has 12 KB or more.
bool writeReport(const Context& ctx, const std::string& text, const SecretTable& secrets) {
  char file[64];
  path(file, sizeof(file), ctx.id, "report.txt");
  HalFile f;
  if (!Storage.openFileForWrite("BUG", file, f)) return false;

  put(f, "CrossDink bug report %s\n\n", ctx.id);
  // The user's own words go through the sanitizer too (a pasted password).
  char clean[MAX_LINE_BYTES + 64];
  const size_t n = BugReportSanitize::sanitizeLine(text.data(), text.size(), clean, sizeof(clean), secrets.list.data(),
                                                   secrets.list.size());
  put(f, "Description:\n");
  f.write(clean, n);
  put(f, "\n\n");

  put(f, "version: %s (%s %s%s, build %s, %s)\n", AppVersion::version(), BuildInfo::shortBranch(), BuildInfo::gitSha(),
      strcmp(BuildInfo::gitDirty(), "1") == 0 ? "-dirty" : "", BuildInfo::buildNumber(), BuildInfo::buildTime());
#ifndef SIMULATOR
  put(f, "board: %s\n", BoardConfig::ACTIVE.name);
  put(f, "reset: %s, wake: %s\n", resetReasonName(esp_reset_reason()), wakeupCauseName(esp_sleep_get_wakeup_cause()));
#endif
  put(f, "uptime: %lu s\n", static_cast<unsigned long>(millis() / 1000));
  const MemoryBudget::HeapSnapshot heap = MemoryBudget::snapshot();
  const MemoryBudget::PsramSnapshot psram = MemoryBudget::psramSnapshot();
  put(f, "heap: free %" PRIu32 " maxAlloc %" PRIu32 ", psram free %" PRIu32 " maxAlloc %" PRIu32 "\n", heap.freeHeap,
      heap.maxAllocHeap, psram.freeHeap, psram.maxAllocHeap);
  put(f, "battery: %u%%\n", static_cast<unsigned>(powerManager.getBatteryPercentage()));
  char row[200];
  lastBatteryRow(row, sizeof(row));
  if (row[0]) put(f, "battery log: %s\n", row);
  if (WiFi.status() == WL_CONNECTED) {
    put(f, "wifi: connected, rssi %d\n", static_cast<int>(WiFi.RSSI()));
  } else {
    put(f, "wifi: not connected\n");
  }

  put(f, "screen: %s\n", ctx.activity);
  if (ctx.book.readerType != ScreenshotInfo::ReaderType::None) {
    put(f, "book: %s\n", ctx.book.title);
    if (ctx.book.spineIndex >= 0) put(f, "chapter: %d\n", ctx.book.spineIndex + 1);
    put(f, "page: %d of %d, %d%% of book\n", ctx.book.currentPage, ctx.book.totalPages, ctx.book.progressPercent);
  }
  const char* font = SETTINGS.sdFontFamilyName[0]
                         ? SETTINGS.sdFontFamilyName
                         : (SETTINGS.fontFamily == CrossPointSettings::BITTER ? "Bitter" : "Lexend Deca");
  put(f, "font: %s %u pt\n", font, static_cast<unsigned>(SETTINGS.readerFontPointSize));
  put(f, "sd fonts: %u\n", static_cast<unsigned>(sdFontSystem.registry().getFamilies().size()));
  library::LibraryIndexFile index;
  if (index.open(library::libraryIndexPath())) put(f, "books: %u\n", static_cast<unsigned>(index.bookCount()));

#if CROSSDINK_GOODIES
  bool knobHeader = false;
  for (int i = 0; i < knobs::COUNT; ++i) {
    const int32_t value = knobs::get(i);
    if (value == knobs::INFO[i].def) continue;
    if (!knobHeader) put(f, "knobs (non-default):\n");
    knobHeader = true;
    put(f, "  %s = %" PRId32 " (default %" PRId32 ")\n", knobs::INFO[i].id, value, knobs::INFO[i].def);
  }
#endif
  const bool ok = f.close();
  if (!ok) LOG_ERR("BUG", "report: write failed %s", file);
  return ok;
}

// --- saving --------------------------------------------------------------

// The log and the zip are most of a save (~2 s of SD work on device), so they
// run on a worker and the screen stays live; loop() shows the outcome toast.
struct SaveJob {
  Context ctx;
  SecretTable secrets;  // built on the main task, freed once the outcome is read
  bool includeLog = false;
  bool ok = false;  // worker writes it; main task reads it once saveTask is idle
};
SaveJob saveJob;
WorkerTask saveTask;
bool saving = false;                             // main task: a save started and its outcome is unread
constexpr uint32_t SAVE_STACK_BYTES = 6 * 1024;  // sanitizer + zip buffers are on the heap

void finishSave(void*) {
  SaveJob& job = saveJob;
  bool ok = !job.includeLog || writeLog(job.ctx, job.secrets);
  if (ok) ok = bundle(job.ctx.id);
  if (!ok) discard(job.ctx);
  LOG_INF("BUG", "report %s %s (log %d)", job.ctx.id, ok ? "saved" : "failed", job.includeLog);
  job.ok = ok;
}

// --- sending -------------------------------------------------------------

#ifndef SIMULATOR
constexpr uint32_t CHECK_EVERY_MS = 5000;
constexpr uint32_t RETRY_AFTER_MS = 5 * 60 * 1000;
constexpr uint32_t TASK_STACK_BYTES = 8 * 1024;  // esp_http_client over plain TCP (no TLS) + 1.6 KB of buffers
// The worker stack is internal RAM; leave room for lwIP's own buffers.
constexpr uint32_t SEND_MIN_FREE = 24 * 1024;
constexpr uint32_t SEND_MIN_BLOCK = 12 * 1024;

enum class Pending : uint8_t { Unknown, Yes, No };
Pending pending = Pending::Unknown;  // main task
uint32_t nextCheckAt = 0;
bool jobStarted = false;  // main task: a send ran and its outcome is unread
WorkerTask sendTask;
std::atomic<bool> sendFailed{false};
std::atomic<bool> stopSending{false};  // deep sleep is coming

bool postZip(const char* base, const char* id, char* buf, const size_t bufLen) {
  char file[64];
  zipPath(file, sizeof(file), id, false);
  HalFile f;
  if (!Storage.openFileForReadIfPresent("BUG", file, f)) return false;
  const size_t size = f.fileSize();
  char url[320];
  snprintf(url, sizeof(url), "%s%cid=%s", base, strchr(base, '?') ? '&' : '?', id);
  esp_http_client_config_t config = {};
  config.url = url;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 5000;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) {
    f.close();
    LOG_ERR("BUG", "send %s: client init failed", id);
    return false;
  }
  esp_http_client_set_header(client, "Content-Type", "application/zip");
  esp_http_client_set_header(client, "User-Agent", AppVersion::userAgent());
  bool ok = esp_http_client_open(client, static_cast<int>(size)) == ESP_OK;
  int r;
  while (ok && !stopSending.load() && (r = f.read(buf, bufLen)) > 0) ok = esp_http_client_write(client, buf, r) == r;
  if (stopSending.load()) ok = false;
  f.close();
  int status = 0;
  if (ok) {
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    ok = status >= 200 && status < 300;
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  if (!ok) LOG_ERR("BUG", "send %s failed (http %d)", id, status);
  return ok;
}

void sendAll(void*) {
  // Unsent reports are <id>.zip (sent ones are <id>.sent.zip); names first so
  // no directory handle stays open while sending.
  std::vector<std::string> ids;
  {
    HalFile dir = Storage.open(DIR);
    if (dir && dir.isDirectory()) {
      char name[48];
      for (HalFile e = dir.openNextFile(); e && ids.size() < 16; e = dir.openNextFile()) {
        const size_t len = e.isDirectory() ? 0 : e.getName(name, sizeof(name));
        if (len > 4 && name[0] != '.' && strcmp(name + len - 4, ".zip") == 0 &&
            !(len > 9 && strcmp(name + len - 9, ".sent.zip") == 0)) {
          ids.emplace_back(name, len - 4);
        }
        e.close();
      }
    }
    dir.close();
  }
  if (ids.empty()) {
    sendFailed.store(false);
    return;
  }

  char base[256] = {};
  HalFile urlFile;
  if (Storage.openFileForReadIfPresent("BUG", URL_PATH, urlFile)) {
    const int n = urlFile.read(base, sizeof(base) - 1);
    urlFile.close();
    size_t end = n > 0 ? static_cast<size_t>(n) : 0;
    base[end] = '\0';
    if (char* nl = strpbrk(base, "\r\n")) *nl = '\0';
  }
  if (strncmp(base, "http://", 7) != 0) {
    LOG_INF("BUG", "%u report(s) queued; put an http:// URL in %s to send them", static_cast<unsigned>(ids.size()),
            URL_PATH);
    sendFailed.store(true);  // retry later: the URL may appear
    return;
  }

  char buf[1024];  // task stack, sized for it
  bool allOk = true;
  int sent = 0;
  for (const std::string& id : ids) {
    if (WiFi.status() != WL_CONNECTED || stopSending.load() || !postZip(base, id.c_str(), buf, sizeof(buf))) {
      allOk = false;
      break;
    }
    char from[64];
    char to[64];
    zipPath(from, sizeof(from), id.c_str(), false);
    zipPath(to, sizeof(to), id.c_str(), true);
    if (!Storage.rename(from, to)) LOG_ERR("BUG", "sent %s but rename failed", id.c_str());
    ++sent;
  }
  if (sent) LOG_INF("BUG", "sent %d report(s)", sent);
  sendFailed.store(!allOk);
}
#endif

}  // namespace

void begin(Activity& source) {
  Context ctx;
  makeId(ctx.id);
  char dir[48];
  workDir(dir, sizeof(dir), ctx.id);
  if (!Storage.ensureDirectoryExists(dir)) {
    LOG_ERR("BUG", "cannot create %s", dir);
    return;
  }
  // The panel is gone: draw the screen it covered before taking the shot.
  activityManager.requestUpdateAndWait();
  snprintf(ctx.activity, sizeof(ctx.activity), "%s", activityManager.currentActivityName());
  ctx.book = activityManager.getScreenshotInfo();
  char shot[64];
  path(shot, sizeof(shot), ctx.id, "screen.png");
  ctx.hasScreenshot = ScreenshotUtil::saveFramebufferAsPng(shot, renderer.getFrameBuffer(), renderer.getDisplayWidth(),
                                                           renderer.getDisplayHeight());
  if (!ctx.hasScreenshot) LOG_ERR("BUG", "screenshot failed");

  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInputManager, tr(STR_DESCRIBE_PROBLEM), "",
                                                           MAX_TEXT, InputType::Text);
  if (!keyboard) {
    LOG_ERR("BUG", "OOM opening keyboard");
    discard(ctx);
    return;
  }
  source.startActivityForResult(std::move(keyboard), [&source, ctx](const ActivityResult& result) {
    if (result.isCancelled) {
      discard(ctx);
      return;
    }
    const auto* kb = std::get_if<KeyboardResult>(&result.data);
    auto confirm =
        makeUniqueNoThrow<BugReportActivity>(renderer, mappedInputManager, ctx, kb ? kb->text : std::string());
    if (!confirm) {
      LOG_ERR("BUG", "OOM opening report sheet");
      discard(ctx);
      return;
    }
    source.startActivityForResult(std::move(confirm), [](const ActivityResult& saved) {
      if (saved.isCancelled) return;
      const auto* outcome = std::get_if<OptionSelectionResult>(&saved.data);
      // Repaint the screen the popup covered; loop() toasts over it once the save is done.
      activityManager.requestUpdateAndWait();
      if (!outcome || !outcome->index) {
        RenderLock lock;
        Toast::show(renderer, tr(STR_REPORT_NOT_SAVED));
      }
    });
  });
}

bool save(const Context& ctx, const std::string& text, const bool includeScreenshot, const bool includeLog) {
  if (saving) {
    LOG_ERR("BUG", "report %s: previous save still running", ctx.id);
    discard(ctx);
    return false;
  }
  saveJob.ctx = ctx;
  saveJob.includeLog = includeLog;
  saveJob.ok = false;
  saveJob.secrets = SecretTable{};
  saveJob.secrets.build();  // reads the stores: main task only
  if (!writeReport(ctx, text, saveJob.secrets)) {
    saveJob.secrets = SecretTable{};
    discard(ctx);
    return false;
  }
  if (!includeScreenshot && ctx.hasScreenshot) {
    char shot[64];
    path(shot, sizeof(shot), ctx.id, "screen.png");
    Storage.remove(shot);
  }
  // SD only, no flash writes: a PSRAM stack is safe (as Library's thumbnail worker).
  if (!saveTask.start(finishSave, nullptr, SAVE_STACK_BYTES, "BugSave", false, WorkerTask::Stack::Psram)) {
    finishSave(nullptr);  // simulator, or no task: inline
  }
  saving = true;
  return true;
}

void discard(const Context& ctx) {
  char dir[48];
  workDir(dir, sizeof(dir), ctx.id);
  Storage.removeDir(dir);
}

void loop() {
  if (saving && !saveTask.running()) {
    saving = false;
    saveJob.secrets = SecretTable{};
    {
      RenderLock lock;
      Toast::show(renderer, saveJob.ok ? tr(STR_REPORT_SAVED) : tr(STR_REPORT_NOT_SAVED));
    }
#ifndef SIMULATOR
    if (saveJob.ok) {
      pending = Pending::Yes;
      nextCheckAt = millis();
    }
#endif
  }
#ifndef SIMULATOR
  if (saving || sendTask.running()) return;
  if (jobStarted) {
    jobStarted = false;
    // Success: nothing left until the next save(). Failure: nextCheckAt already holds the retry.
    if (!sendFailed.load()) pending = Pending::No;
  }
  if (pending == Pending::No) return;
  const uint32_t now = millis();
  if (static_cast<int32_t>(now - nextCheckAt) < 0) return;
  nextCheckAt = now + CHECK_EVERY_MS;
  if (WiFi.status() != WL_CONNECTED) return;
  if (pending == Pending::Unknown) {
    // First Wi-Fi link since boot: any report folder at all?
    pending = Storage.exists(DIR) ? Pending::Yes : Pending::No;
    if (pending == Pending::No) return;
  }
  if (!MemoryBudget::hasHeap(MemoryBudget::snapshot(), SEND_MIN_FREE, SEND_MIN_BLOCK)) return;
  nextCheckAt = now + RETRY_AFTER_MS;  // after a failure; success clears pending below
  sendFailed.store(false);
  stopSending.store(false);
  if (!sendTask.start(sendAll, nullptr, TASK_STACK_BYTES, "BugReport")) {
    LOG_ERR("BUG", "send task did not start");
    return;
  }
  jobStarted = true;  // read the outcome once the task is done
#endif
}

void stopForSleep() {
  // A save is short and its zip half-written is lost: let it finish.
  if (saveTask.running() && !saveTask.join(8000)) LOG_ERR("BUG", "save task still running at sleep");
#ifndef SIMULATOR
  if (!sendTask.running()) return;
  stopSending.store(true);
  if (!sendTask.join(8000)) LOG_ERR("BUG", "send task still running at sleep");
#endif
}

}  // namespace BugReport
