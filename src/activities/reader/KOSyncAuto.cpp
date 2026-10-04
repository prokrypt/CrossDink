#include "KOSyncAuto.h"

#include <Epub.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <memory>

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
// As the KOSync screen's request task: wolfSSL handshake plus HTTPClient.
constexpr uint32_t TASK_STACK_BYTES = 14 * 1024;

std::string queuedPath;  // main task only
uint32_t queuedAt = 0;
std::string pullPath;  // main task only
uint32_t pullAt = 0;
// Written before the task starts, then the task's until running() reads false.
std::string jobPath;
bool jobIsPull = false;
bool pullReady = false;
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
  const char* what = jobIsPull ? "open pull" : "exit push";
  KOReaderProgress progress;
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
  }

  bool ownRadio = false;
  if (!hasActiveStationWifiConnection()) {
#if CROSSDINK_GOODIES
    if (goodies_remote::wanted()) {
      LOG_INF("KOSync", "%s skipped: Wi-Fi remote owns the radio and is not connected", what);
      return;
    }
#endif
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

  KOReaderSyncClient::Error result = KOReaderSyncClient::NETWORK_ERROR;
  if (WiFi.status() == WL_CONNECTED && !radioClaimed.load()) {
    result = jobIsPull ? fetch(hash, altHash, method)
                       : retriedOnce([&] { return KOReaderSyncClient::updateProgress(progress); });
  }

  if (ownRadio && beginRadioCall()) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    radioCall.store(false);
  }
  pullReady = jobIsPull && result == KOReaderSyncClient::OK;
  LOG_INF("KOSync", "%s %s: result=%d http=%d pct=%.4f claimed=%d %lu ms", what,
          result == KOReaderSyncClient::OK ? "ok" : "failed", result, KOReaderSyncClient::lastHttpCode,
          jobIsPull ? pulled.percentage : progress.percentage, radioClaimed.load() ? 1 : 0,
          static_cast<unsigned long>(millis() - start));
}

bool startJob(std::string path, const bool pull) {
  if (!MemoryBudget::hasHeapForOptionalEpubRebuild("KOSync", pull ? "open pull" : "exit push", -1)) return false;
  jobPath = std::move(path);
  jobIsPull = pull;
  pullReady = false;
  radioClaimed.store(false);
  if (task.start(run, nullptr, TASK_STACK_BYTES, "KOSyncAuto")) return true;
  LOG_ERR("KOSync", "auto sync: task did not start");
  return false;
}
}  // namespace

namespace kosync_auto {
void queuePull(const std::string& epubPath) {
  if (!(SETTINGS.koAutoSync & CrossPointSettings::KO_AUTO_SYNC_OPEN) || !KOREADER_STORE.hasCredentials()) return;
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

void loop() {
  if (task.running()) return;  // a push or fetch waits for the one before it
  if (!queuedPath.empty() && millis() - queuedAt >= START_DELAY_MS) {
    std::string path = std::move(queuedPath);
    queuedPath.clear();
    // Another book, or a screen with its own network (the KOSync screen included).
    if (activityManager.isReaderActivity() || activityManager.anyActivityUsesWifi()) {
      LOG_INF("KOSync", "exit push dropped: reader or Wi-Fi screen is up");
      return;
    }
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

void yieldRadio() {
  if (!task.running()) return;
  radioClaimed.store(true);
  while (radioCall.load()) vTaskDelay(1);
}
}  // namespace kosync_auto
