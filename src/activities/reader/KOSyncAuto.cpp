#include "KOSyncAuto.h"

#include <Epub.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "EpubReaderUtils.h"
#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "network/WifiUtils.h"
#include "util/WorkerTask.h"
#if CROSSDINK_GOODIES
#include "activities/goodies/GoodiesActivity.h"
#endif

namespace {
// Home (or the book's first page) paints and the reader's exit writes land before the task reads the SD card.
constexpr uint32_t START_DELAY_MS = 1500;
constexpr uint32_t JOIN_TIMEOUT_MS = 15000;
constexpr uint32_t REMOTE_WAIT_MS = 20000;       // the Wi-Fi remote's own join after boot or wake
constexpr float SAME_PROGRESS_EPSILON = 0.001f;  // as Smart Sync
// Deep sleep's whole wait for a running job and its own push; the sleep guard resets at 60 s.
constexpr uint32_t SLEEP_WAIT_MS = 30000;
// As the KOSync screen's request task: wolfSSL handshake plus HTTPClient.
constexpr uint32_t TASK_STACK_BYTES = 14 * 1024;
// Sized for this job, not an EPUB rebuild (whose 96K/48K gate skipped every sync
// beside a book): the client's TLS gate (35K free, 20K block) plus the task
// stack, and headroom for starting Wi-Fi, whose buffers can sit in PSRAM.
// The end-of-job line logs internal heap at start, after the join and at the end.
KNOB_ALIAS(MIN_FREE, koSyncMinFree);  // Goodies > Knobs
KNOB_ALIAS(MIN_BLOCK, koSyncMinBlock);

std::string queuedPath;  // main task only
uint32_t queuedAt = 0;
std::string pullPath;  // main task only
uint32_t pullAt = 0;
bool wakePull = false;  // main task only
bool wakePush = false;  // main task only
// A failed sleep push leaves this for the next wake, which then pushes as well as
// pulls. RTC memory; power loss leaves garbage that fails the magic check.
constexpr uint32_t RETRY_PUSH_MAGIC = 0x4B535250;  // "KSRP"
RTC_NOINIT_ATTR uint32_t retryPushMagic;
// Written before the task starts, then the task's until running() reads false.
std::string jobPath;
bool jobIsPull = false;
bool pullReady = false;
bool pushOk = false;  // the task's last push landed; main task reads it once running() is false
// The last push's outcome, written by the task before running() reads false.
kosync_auto::PushOutcome pushOutcome = kosync_auto::PushOutcome::Failed;
KOReaderProgress pulled;
DocumentMatchMethod pulledMethod = DocumentMatchMethod::FILENAME;
WorkerTask task;
// yieldRadio() and the task's Wi-Fi start/stop pair up as a Dekker handshake
// (seq_cst): either the task sees the claim and keeps off the radio, or the
// claimer waits for the task's call to finish.
std::atomic<bool> radioClaimed{false};
std::atomic<bool> radioCall{false};

bool beginRadioCall() {
  radioCall.store(true);
  if (!radioClaimed.load()) return true;
  radioCall.store(false);
  return false;
}

// Same mapping as the KOSync screen's upload of saved progress.
bool buildProgress(const std::string& path, KOReaderProgress& out) {
  const DocumentMatchMethod method = KOREADER_STORE.getMatchMethod();
  auto epub = std::make_shared<Epub>(path, "/.crossdink");
  epub->setupCacheDir();
  if (!epub->load(false, true, Epub::XLocationLoadMode::Immediate, true)) {
    LOG_ERR("KOSync", "exit push: epub load failed for %s", path.c_str());
    return false;
  }
  EpubReaderUtils::Progress saved;
  if (!EpubReaderUtils::loadProgress(*epub, saved, "KOSync")) {
    LOG_ERR("KOSync", "exit push: no saved progress for %s", path.c_str());
    return false;
  }
  const int spine = saved.spineIndex >= 0 && saved.spineIndex < epub->getSpineItemsCount() ? saved.spineIndex : 0;
  const int pages = saved.hasPageCount ? std::max(1, saved.pageCount) : 1;
  CrossPointPosition pos = {spine, saved.pageNumber, pages};
  if (saved.hasVisibleTextOffset) {
    pos.visibleTextOffset = saved.visibleTextOffset;
    pos.hasVisibleTextOffset = true;
  }
  const KOReaderPosition ko =
      ProgressMapper::toKOReader(epub, pos,
                                 method == DocumentMatchMethod::FILENAME ? PositionCoordinateSpace::SourceDocument
                                                                         : PositionCoordinateSpace::CurrentDocument);
  if (!ko.valid) {
    LOG_ERR("KOSync", "exit push: no source position map; re-optimize the EPUB");
    return false;
  }
  out.document = method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                         : KOReaderDocumentId::calculate(path);
  if (out.document.empty()) {
    LOG_ERR("KOSync", "exit push: document hash failed");
    return false;
  }
  out.progress = ko.xpath;
  out.percentage = ko.percentage;
  out.device = SETTINGS.getEffectiveDeviceName();
  if (KOREADER_STORE.getSendMetadata()) {
    KOReaderMetadata meta;
    const auto slash = path.rfind('/');
    meta.filename = slash != std::string::npos ? path.substr(slash + 1) : path;
    meta.title = epub->getTitle();
    meta.authors = epub->getAuthor();
    out.metadata = std::move(meta);
  }
  if (KOREADER_STORE.usesCrossPointSyncServer()) {
    KOReaderRichPosition rich;
    rich.pctQ = static_cast<uint32_t>(std::clamp(ko.percentage, 0.0f, 1.0f) * 1000000.0f + 0.5f);
    rich.spineIndex = static_cast<uint16_t>(spine);
    rich.pageNumber = static_cast<uint16_t>(saved.pageNumber);
    rich.totalPages = static_cast<uint16_t>(pages);
    rich.xpath = ko.xpath;
    out.position = std::move(rich);
  }
  return true;
}

std::string documentId(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}

DocumentMatchMethod otherMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

// One retry on a fresh connection, as the KOSync screen does.
template <typename Request>
KOReaderSyncClient::Error retriedOnce(Request&& request) {
  KOReaderSyncClient::Error result = request();
  if (result == KOReaderSyncClient::NETWORK_ERROR && !radioClaimed.load()) result = request();
  return result;
}

// Smart Sync's fetch: the configured hash, then the other method's, keeping the
// furthest record (the KOSync screen's finishSync() rule).
KOReaderSyncClient::Error fetch(const std::string& hash, const std::string& altHash, const DocumentMatchMethod method) {
  KOReaderProgress remote;
  KOReaderSyncClient::Error result = retriedOnce([&] { return KOReaderSyncClient::getProgress(hash, remote); });
  pulledMethod = method;
  if (!altHash.empty() && altHash != hash && !radioClaimed.load() && result != KOReaderSyncClient::NETWORK_ERROR &&
      result != KOReaderSyncClient::AUTH_FAILED && result != KOReaderSyncClient::LOW_MEMORY) {
    KOReaderProgress alt;
    if (KOReaderSyncClient::getProgress(altHash, alt) == KOReaderSyncClient::OK &&
        (result == KOReaderSyncClient::NOT_FOUND || alt.percentage > remote.percentage)) {
      remote = std::move(alt);
      pulledMethod = otherMethod(method);
      result = KOReaderSyncClient::OK;
    }
  }
  if (result == KOReaderSyncClient::OK) pulled = std::move(remote);
  return result;
}

void run(void*) {
  const uint32_t start = millis();
  const MemoryBudget::HeapSnapshot heapStart = MemoryBudget::snapshot();
  const char* what = jobIsPull ? "open pull" : "exit push";
  KOReaderProgress progress;
  bool skipped = false;
  std::string hash, altHash;
  const DocumentMatchMethod method = KOREADER_STORE.getMatchMethod();
  if (jobIsPull) {
    hash = documentId(jobPath, method);
    altHash = documentId(jobPath, otherMethod(method));
    if (hash.empty()) {
      LOG_ERR("KOSync", "open pull: document hash failed");
      return;
    }
  } else if (!buildProgress(jobPath, progress)) {
    return;  // the Epub is freed here, before TLS
  } else {
    altHash = documentId(jobPath, otherMethod(method));  // SD reads before the radio comes up
  }

#if CROSSDINK_GOODIES
  // The Wi-Fi remote owns the radio and may still be joining (boot, wake): wait
  // for its link instead of skipping. Bounded, and on this task, never the UI's.
  if (goodies_remote::wanted() && !hasActiveStationWifiConnection()) {
    const uint32_t waitStart = millis();
    while (goodies_remote::wanted() && !hasActiveStationWifiConnection() && !radioClaimed.load() &&
           millis() - waitStart < REMOTE_WAIT_MS) {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (!hasActiveStationWifiConnection()) {
      LOG_INF("KOSync", "%s skipped: Wi-Fi remote not connected after %lu ms", what,
              static_cast<unsigned long>(millis() - waitStart));
      return;
    }
    LOG_INF("KOSync", "%s: waited %lu ms for the Wi-Fi remote", what, static_cast<unsigned long>(millis() - waitStart));
  }
#endif
  bool ownRadio = false;
  if (!hasActiveStationWifiConnection()) {
    auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
    if (!cred) {
      LOG_INF("KOSync", "%s skipped: no saved Wi-Fi network", what);
      return;
    }
    if (!beginRadioCall()) return;
    WiFi.persistent(false);
    ownRadio = WiFi.mode(WIFI_STA);
    if (ownRadio) WiFi.begin(cred->ssid.c_str(), cred->password.empty() ? nullptr : cred->password.c_str());
    radioCall.store(false);
    if (!ownRadio) {
      LOG_ERR("KOSync", "%s: station mode failed", what);
      return;
    }
    const uint32_t joinStart = millis();
    while (WiFi.status() != WL_CONNECTED && !radioClaimed.load() && millis() - joinStart < JOIN_TIMEOUT_MS) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }

  const MemoryBudget::HeapSnapshot heapJoined = MemoryBudget::snapshot();
  KOReaderSyncClient::Error result = KOReaderSyncClient::NETWORK_ERROR;
  if (WiFi.status() == WL_CONNECTED && !radioClaimed.load()) {
    if (jobIsPull) {
      result = fetch(hash, altHash, method);
    } else {
      // Never move the server back: fetch first (both hashes, as Smart Sync) and
      // push only when nothing is there or the device is further on.
      result = fetch(progress.document, altHash, method);
      if (result == KOReaderSyncClient::OK && pulled.percentage + SAME_PROGRESS_EPSILON >= progress.percentage) {
        LOG_INF("KOSync", "%s skipped: server at %.4f (%s), device at %.4f", what, pulled.percentage,
                pulled.device.c_str(), progress.percentage);
        skipped = true;
        pushOutcome = pulled.percentage - progress.percentage > SAME_PROGRESS_EPSILON
                          ? kosync_auto::PushOutcome::ServerAhead
                          : kosync_auto::PushOutcome::Same;
      } else if ((result == KOReaderSyncClient::OK || result == KOReaderSyncClient::NOT_FOUND) &&
                 !radioClaimed.load()) {
        result = retriedOnce([&] { return KOReaderSyncClient::updateProgress(progress); });
        pushOk = result == KOReaderSyncClient::OK;
        if (pushOk) pushOutcome = kosync_auto::PushOutcome::Pushed;
      }
    }
  }

  if (ownRadio && beginRadioCall()) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    radioCall.store(false);
  }
  pullReady = jobIsPull && result == KOReaderSyncClient::OK;
  const MemoryBudget::HeapSnapshot heapEnd = MemoryBudget::snapshot();
  LOG_INF("KOSync",
          "%s %s: result=%d http=%d pct=%.4f claimed=%d %lu ms heap free/block start %" PRIu32 "/%" PRIu32
          " joined %" PRIu32 "/%" PRIu32 " end %" PRIu32 "/%" PRIu32 " stack free %u",
          what,
          skipped                            ? "skipped"
          : result == KOReaderSyncClient::OK ? "ok"
                                             : "failed",
          result, KOReaderSyncClient::lastHttpCode, jobIsPull ? pulled.percentage : progress.percentage,
          radioClaimed.load() ? 1 : 0, static_cast<unsigned long>(millis() - start), heapStart.freeHeap,
          heapStart.maxAllocHeap, heapJoined.freeHeap, heapJoined.maxAllocHeap, heapEnd.freeHeap, heapEnd.maxAllocHeap,
          static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
}

bool startJob(std::string path, const bool pull) {
  const MemoryBudget::HeapSnapshot heap = MemoryBudget::snapshot();
  if (!MemoryBudget::hasHeap(heap, MIN_FREE, MIN_BLOCK)) {
    LOG_INF("KOSync", "%s skipped: low heap (free=%" PRIu32 " block=%" PRIu32 ", need %" PRIu32 "/%" PRIu32 ")",
            pull ? "open pull" : "exit push", heap.freeHeap, heap.maxAllocHeap, static_cast<uint32_t>(MIN_FREE),
            static_cast<uint32_t>(MIN_BLOCK));
    return false;
  }
  jobPath = std::move(path);
  jobIsPull = pull;
  pullReady = false;
  pushOk = false;
  pushOutcome = kosync_auto::PushOutcome::Failed;
  radioClaimed.store(false);
  if (task.start(run, nullptr, TASK_STACK_BYTES, "KOSyncAuto")) return true;
  LOG_ERR("KOSync", "auto sync: task did not start");
  return false;
}

kosync_auto::PushOutcome sleepPush(std::string epubPath) {
  if (queuedPath == epubPath) queuedPath.clear();  // this push replaces a pending At close one
  const uint32_t start = millis();
  kosync_auto::yieldRadio();  // an open pull still waiting on Wi-Fi gives up now
  // Every wait is bounded: the job's own Wi-Fi join and HTTP calls time out, and
  // past SLEEP_WAIT_MS the caller moves on with the job cut off from the radio.
  if (!task.join(SLEEP_WAIT_MS)) {
    LOG_ERR("KOSync", "sleep push: earlier job still running after %lu ms", static_cast<unsigned long>(SLEEP_WAIT_MS));
    kosync_auto::yieldRadio();
    return kosync_auto::PushOutcome::Failed;
  }
  if (!startJob(std::move(epubPath), false)) return kosync_auto::PushOutcome::Failed;
  const uint32_t used = millis() - start;
  if (!task.join(used < SLEEP_WAIT_MS ? SLEEP_WAIT_MS - used : 0)) {
    LOG_ERR("KOSync", "sleep push: still running after %lu ms, sleeping anyway",
            static_cast<unsigned long>(SLEEP_WAIT_MS));
    kosync_auto::yieldRadio();
    return kosync_auto::PushOutcome::Failed;
  }
  pushOk = false;  // the sleep toast reports it; no main-loop toast after wake
  return pushOutcome;
}

}  // namespace

namespace kosync_auto {
void noteWake() {
  wakePull = SETTINGS.koSyncSleepWake != 0;
  wakePush = wakePull && retryPushMagic == RETRY_PUSH_MAGIC;
  retryPushMagic = 0;
}

void queuePull(const std::string& epubPath) {
  queuedAt = millis();  // a pending push also waits out the book's open
  // ponytail: a wake whose reader never reaches here leaves the flag for the next open (one extra prompted pull).
  const bool wake = std::exchange(wakePull, false);
  // The push's own server check makes its order against the pull irrelevant.
  if (std::exchange(wakePush, false) && KOREADER_STORE.hasCredentials()) {
    LOG_INF("KOSync", "wake: retrying the failed sleep push");
    queuedPath = epubPath;
  }
  if (!(wake || (SETTINGS.koAutoSync & CrossPointSettings::KO_AUTO_SYNC_OPEN)) || !KOREADER_STORE.hasCredentials())
    return;
  pullPath = epubPath;
  pullAt = millis();
}

void queue(const std::string& epubPath) {
  pullPath.clear();
  if (!(SETTINGS.koAutoSync & CrossPointSettings::KO_AUTO_SYNC_CLOSE) || !KOREADER_STORE.hasCredentials()) return;
  queuedPath = epubPath;
  queuedAt = millis();
}

const KOReaderProgress* takePull(const std::string& epubPath, DocumentMatchMethod& method) {
  if (!pullReady || task.running()) return nullptr;
  pullReady = false;
  if (epubPath != jobPath) return nullptr;
  method = pulledMethod;
  return &pulled;
}

bool takePushed() {
  if (!pushOk || task.running()) return false;
  pushOk = false;
  return true;
}

void loop() {
  if (task.running()) return;  // a push or fetch waits for the one before it
  if (!queuedPath.empty() && millis() - queuedAt >= START_DELAY_MS) {
    // A screen with its own network (the KOSync screen included) keeps it queued.
    if (activityManager.anyActivityUsesWifi()) return;
    std::string path = std::move(queuedPath);
    queuedPath.clear();
    startJob(std::move(path), false);
    return;
  }
  // The reader drops pullPath on exit, so a fetch only starts beside its book.
  if (!pullPath.empty() && millis() - pullAt >= START_DELAY_MS && !activityManager.anyActivityUsesWifi()) {
    std::string path = std::move(pullPath);
    pullPath.clear();
    startJob(std::move(path), true);
  }
}

bool wantsSleepPush() {
  return SETTINGS.koSyncSleepWake && KOREADER_STORE.hasCredentials() && !activityManager.anyActivityUsesWifi();
}

PushOutcome pushNow(std::string epubPath) {
  const PushOutcome outcome = sleepPush(std::move(epubPath));
  retryPushMagic = outcome == PushOutcome::Failed ? RETRY_PUSH_MAGIC : 0;
  return outcome;
}

void yieldRadio() {
  if (!task.running()) return;
  radioClaimed.store(true);
  while (radioCall.load()) vTaskDelay(1);
}
}  // namespace kosync_auto
