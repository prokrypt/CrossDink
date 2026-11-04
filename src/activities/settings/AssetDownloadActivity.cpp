#include "AssetDownloadActivity.h"

#include <AppVersion.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

// Deployment configuration is deliberately empty until official endpoints have
// been approved and qualified. Catalog data cannot select a host or redirect.
#ifndef ASSET_DOWNLOAD_BASE_URL
#define ASSET_DOWNLOAD_BASE_URL ""
#endif
#ifndef ASSET_CATALOG_BASE_URL
#define ASSET_CATALOG_BASE_URL ASSET_DOWNLOAD_BASE_URL
#endif
#ifndef ASSET_DOWNLOAD_REDIRECT_BASE_URL
#define ASSET_DOWNLOAD_REDIRECT_BASE_URL ""
#endif

using AssetDownloadService::Result;
using AssetDownloadService::Status;

// Bit 8 marks a translation payload, as upstream's catalog payloads do.
uint32_t AssetDownloadActivity::bootPayload(uint8_t languageIndex) { return 0x100U | languageIndex; }
bool AssetDownloadActivity::validBootPayload(uint32_t payload) {
  // Only a registry index crosses reboot; never an untrusted path or pointer.
  return (payload & 0x100U) && payload < 0x200U && (payload & 0xFFU) > 0 && (payload & 0xFFU) < getLanguageCount();
}
AssetDownloadActivity::AssetDownloadActivity(GfxRenderer& renderer, MappedInputManager& input, uint32_t payload)
    : Activity("AssetDownload", renderer, input),
      wantedIndex_(payload & 0xFFU),
      endpoints_{ASSET_DOWNLOAD_BASE_URL, ASSET_DOWNLOAD_REDIRECT_BASE_URL},
      catalogEndpoints_{ASSET_CATALOG_BASE_URL, ASSET_DOWNLOAD_REDIRECT_BASE_URL} {}
namespace {
constexpr StrId title() { return StrId::STR_DOWNLOAD_LANGUAGES; }
}  // namespace
void AssetDownloadActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);
  // Fixed <13 KiB catalog, one per activity. Too large for the C3 task stack;
  // freeing on exit keeps offline reading free of this network-only allocation.
  catalog_ = makeUniqueNoThrow<AssetCatalog::Catalog>();
  if (!catalog_) {
    LOG_ERR("ASSET", "OOM: %u byte catalog", unsigned(sizeof(AssetCatalog::Catalog)));
    showError(StrId::STR_MEMORY_ERROR, Pending::Exit);
    return;
  }
  if (!endpoints_.base[0] || !catalogEndpoints_.base[0]) {
    showError(StrId::STR_ASSET_UNAVAILABLE, Pending::Exit);
    return;
  }
  auto wifi = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifi) {
    LOG_ERR("ASSET", "OOM: Wi-Fi selection");
    showError(StrId::STR_MEMORY_ERROR, Pending::Exit);
    return;
  }
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::move(wifi), [this](const ActivityResult& result) {
    pending_ = result.isCancelled ? Pending::Exit : Pending::Fetch;
  });
}
void AssetDownloadActivity::onExit() {
  // ActivityManager already owns its non-recursive render lock in onExit.
  popup_.close();
  catalog_.reset();
  catalogReady_ = false;
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  // The minimal network boot skipped the SD fonts; restore them after the
  // catalog storage is released, as Manage Fonts does on its way out.
  sdFontSystem.releaseRegistry();
  sdFontSystem.ensureLoaded(renderer);
  sdFontSystem.releaseRegistry();
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}
bool AssetDownloadActivity::pollCancel() {
  if (cancelled_) return true;
  mappedInput.update();
  if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    mappedInput.suppressNextBackRelease();
    cancelled_ = true;
  }
  if (mappedInput.wasHomeGesture() || TouchHeaderBackButton::wasTapped(mappedInput, renderer)) cancelled_ = true;
  // The same popup handles button and touch cancellation during blocking I/O.
  {
    RenderLock lock(*this);
    popup_.handleInput(mappedInput, [this] { requestUpdate(); });
  }
  return cancelled_;
}
AssetDownloadService::Callbacks AssetDownloadActivity::callbacks() {
  return {this, [](void* context) { return static_cast<AssetDownloadActivity*>(context)->pollCancel(); },
          [](void* context, size_t received, size_t total) {
            static_cast<AssetDownloadActivity*>(context)->progress(received, total);
          },
          [](void* context) { static_cast<AssetDownloadActivity*>(context)->verifying(); },
          [](void* context) { static_cast<AssetDownloadActivity*>(context)->payloadStarted(); }};
}
OptionPopup::Note AssetDownloadActivity::busyNote() {
  if (!step_) return OptionPopup::Note(I18N.get(phase_), progress_);
  constexpr uint8_t STEP_COUNT = 3;
  const StrId names[STEP_COUNT] = {StrId::STR_ASSET_STEP_CATALOG, StrId::STR_ASSET_STEP_LICENSES, StrId::STR_LANGUAGE};
  char step[16];
  std::snprintf(step, sizeof(step), tr(STR_ASSET_STEP_FORMAT), unsigned(step_), unsigned(STEP_COUNT));
  // One two-line note: "Step 2/3: Licenses" over the phase and byte count.
  std::snprintf(stepLabel_, sizeof(stepLabel_), "%s: %s", step, I18N.get(names[step_ - 1]));
  std::snprintf(progressLine_, sizeof(progressLine_), "%s %s", I18N.get(phase_), progress_);
  return OptionPopup::Note(stepLabel_, progressLine_);
}
void AssetDownloadActivity::showBusy(StrId phase) {
  {
    RenderLock lock(*this);
    cancelled_ = false;
    busy_ = true;
    phase_ = phase;
    progress_[0] = 0;
    lastProgressAt_ = 0;
    const StrId options[] = {StrId::STR_CANCEL};
    popup_.show(title(), options, 1, 0, [this](int) { cancelled_ = true; }, busyNote());
    popup_.setCancelCallback([this] { cancelled_ = true; });
  }
  requestUpdateAndWait();
}
void AssetDownloadActivity::payloadStarted() {
  // Replace the companion popup outside its callbacks; keep cancellation available.
  const bool wasCancelled = cancelled_;
  step_ = 3;
  showBusy(StrId::STR_DOWNLOADING);
  cancelled_ = wasCancelled;
}
void AssetDownloadActivity::progress(size_t received, size_t total) {
  if (phase_ == StrId::STR_ASSET_VERIFYING) {
    const bool wasCancelled = cancelled_;
    showBusy(StrId::STR_DOWNLOADING);
    cancelled_ = wasCancelled;
  }
  const uint32_t now = millis();
  if (lastProgressAt_ && now - lastProgressAt_ < 750 && received != total) return;
  lastProgressAt_ = now;
  {
    RenderLock lock(*this);
    char done[16];
    StringUtils::formatByteSize(received, done, sizeof(done));
    if (total) {
      char size[16];
      StringUtils::formatByteSize(total, size, sizeof(size));
      const unsigned percent = unsigned((uint64_t(received) * 100) / total);
      std::snprintf(progress_, sizeof(progress_), "%u%%  %s / %s", percent, done, size);
    } else {
      std::snprintf(progress_, sizeof(progress_), "%s", done);
    }
  }
  requestUpdate(true);
}
void AssetDownloadActivity::verifying() {
  // Replace the popup outside its callbacks; keep cancellation available.
  const bool wasCancelled = cancelled_;
  showBusy(StrId::STR_ASSET_VERIFYING);
  cancelled_ = wasCancelled;
}
void AssetDownloadActivity::showError(StrId error, Pending next) {
  RenderLock lock(*this);
  busy_ = false;
  const bool retry = next == Pending::Fetch || next == Pending::Install;
  const StrId options[] = {retry ? StrId::STR_CANCEL : StrId::STR_OK, StrId::STR_RETRY};
  popup_.show(
      title(), options, retry ? 2 : 1, 0,
      [this, next, retry](int index) { pending_ = retry && index == 0 ? Pending::Exit : next; },
      OptionPopup::Note(error == StrId::STR_ASSET_CURRENT ? tr(STR_INSTALLED) : tr(STR_DOWNLOAD), I18N.get(error)));
  popup_.setCancelCallback([this, next, retry] { pending_ = retry ? Pending::Exit : next; });
  requestUpdate();
}
void AssetDownloadActivity::fetch() {
  catalogReady_ = false;
  step_ = 1;
  showBusy();
  AssetCatalog::Options parserOptions;
  parserOptions.expectedKind = kind_;
  parserOptions.expectedChannel = AssetDownloadService::channelForVersion(AppVersion::version());
  parserOptions.expectedSource = I18n::sourceContract();
  parserOptions.expectedKeyset = I18n::keysetContract();
  parserOptions.supportedTranslation = [](const char* code, void*) { return I18n::supportedDownload(code); };
  // Parser scratch is bounded but larger than the preferred stack budget.
  // It is released before any payload transfer or source validation.
  auto parser = makeUniqueNoThrow<AssetCatalog::Parser>(*catalog_, parserOptions);
  if (!parser) {
    LOG_ERR("ASSET", "OOM: catalog parser");
    showError(StrId::STR_MEMORY_ERROR, Pending::Exit);
    return;
  }
  char relative[AssetCatalog::MAX_CATALOG_PATH];
  bool valid = AssetCatalog::catalogPath(parserOptions, relative, sizeof(relative)) &&
               AssetDownloadService::buildUrl(catalogEndpoints_, relative, url_, sizeof(url_));
  if (valid) {
    LOG_DBG("ASSET", "Fetching catalog: %s", url_);
    HttpDownloader::DownloadOptions options;
    options.maxBytes = AssetCatalog::MAX_CATALOG_BYTES;
    options.maxRedirects = 2;
    options.allowUrl = AssetDownloadService::allowUrl;
    options.urlContext = &catalogEndpoints_;
    options.transport = HttpDownloader::Transport::ESP_HTTP;
    options.shouldCancel = [this] { return pollCancel(); };
    const auto result = HttpDownloader::streamUrl(
        url_,
        [&parser](const uint8_t* bytes, size_t count) {
          return parser->feed(reinterpret_cast<const char*>(bytes), count);
        },
        [this](size_t received, size_t total) { progress(received, total); }, "", "", options);
    valid = result == HttpDownloader::OK && parser->finish();
  }
  if (!valid) LOG_ERR("ASSET", "Catalog rejected: %s", parser->error() ? parser->error() : "transport");
  const bool incompatible = parser->error() != nullptr;
  parser.reset();
  busy_ = false;
  if (cancelled_) {
    pending_ = Pending::Exit;
    return;
  }
  if (!valid) {
    showError(incompatible ? StrId::STR_ASSET_CATALOG_INVALID : StrId::STR_DOWNLOAD_FAILED, Pending::Fetch);
    return;
  }
  if (!catalog_->count) {
    showError(StrId::STR_ASSET_UNAVAILABLE, Pending::Exit);
    return;
  }
  catalogReady_ = true;
  // Only the one language chosen before the restart is installed. Translation
  // catalog codes use the same exact spelling as LANGUAGE_CODES.
  const char* wanted = wantedIndex_ < getLanguageCount() ? LANGUAGE_CODES[wantedIndex_] : nullptr;
  size_t i = 0;
  while (wanted && i < catalog_->count && std::strcmp(wanted, catalog_->entries[i].code)) ++i;
  if (!wanted || i == catalog_->count) {
    showError(StrId::STR_ASSET_UNAVAILABLE, Pending::Exit);
    return;
  }
  selected_ = static_cast<int>(i);
  pending_ = Pending::Install;
}
void AssetDownloadActivity::confirmOverwrite() {
  RenderLock lock(*this);
  const StrId options[] = {StrId::STR_CANCEL, StrId::STR_DOWNLOAD};
  popup_.show(
      title(), options, 2, 0,
      [this](int index) {
        overwriteApproved_ = index == 1;
        pending_ = index == 1 ? Pending::Download : Pending::Exit;
      },
      OptionPopup::Note(tr(STR_DOWNLOAD), tr(STR_ASSET_OVERWRITE_HINT)));
  popup_.setCancelCallback([this] { pending_ = Pending::Exit; });
  requestUpdate();
}
void AssetDownloadActivity::installSelected() {
  step_ = 0;  // Local SD check only; no transfer.
  showBusy(StrId::STR_ASSET_VERIFYING);
  const auto result = AssetDownloadService::discover(kind_, catalog_->entries[selected_], source_, callbacks());
  busy_ = false;
  if (cancelled_) {
    pending_ = Pending::Exit;
    return;
  }
  if (result != Result::Ok) {
    LOG_ERR("ASSET", "Pre-download SD check failed: code=%s result=%s", catalog_->entries[selected_].code,
            AssetDownloadService::resultName(result));
    showError(result == Result::Collision ? StrId::STR_LANGUAGE_DUPLICATE : StrId::STR_LANGUAGE_IO_ERROR,
              Pending::Install);
    return;
  }
  overwriteApproved_ = false;
  pending_ = source_.status == Status::Current && !source_.recoveryPending                 ? Pending::Activate
             : (source_.status == Status::Different || source_.status == Status::FileOnSd) ? Pending::Overwrite
                                                                                           : Pending::Download;
}
void AssetDownloadActivity::download() {
  step_ = 2;  // Companion indexes and files; payloadStarted() advances to the payload.
  showBusy(StrId::STR_DOWNLOADING);
  const auto result = AssetDownloadService::download(*catalog_, catalog_->entries[selected_], endpoints_,
                                                     overwriteApproved_, callbacks());
  busy_ = false;
  overwriteApproved_ = false;
  if (result == Result::Cancelled) {
    pending_ = Pending::Exit;
    return;
  }
  if (result == Result::OverwriteRequired) {
    pending_ = Pending::Overwrite;
    return;
  }
  if (result != Result::Ok) {
    LOG_ERR("ASSET", "Download: %s", AssetDownloadService::resultName(result));
    showError(result == Result::Collision ? StrId::STR_LANGUAGE_DUPLICATE : StrId::STR_DOWNLOAD_FAILED,
              Pending::Install);
    return;
  }
  pending_ = Pending::Activate;  // Install flow: no separate activation choice.
}
void AssetDownloadActivity::activate() {
  // Recheck the local source before activation; download never selects it.
  step_ = 0;
  showBusy(StrId::STR_ASSET_VERIFYING);
  const auto& entry = catalog_->entries[selected_];
  const auto found = AssetDownloadService::discover(kind_, entry, source_, callbacks());
  busy_ = false;
  if (cancelled_) {
    pending_ = Pending::Exit;
    return;
  }
  if (found != Result::Ok || source_.status == Status::Missing || source_.status == Status::Collision) {
    showError(StrId::STR_LANGUAGE_FILE_INVALID, Pending::Install);
    return;
  }
  RenderLock lock(*this);
  popup_.close();
  GUI.drawPopup(renderer, tr(STR_ASSET_INSTALLING));
  // Same install/save contract as Settings' offline language picker: the SD
  // cache slot is compiled and switched live, no restart.
  language_cache::Installed installed;
  const auto result = I18N.install(source_.path, installed);
  if (result == language_cache::Result::Unchanged) {
    lock.unlock();
    showError(StrId::STR_ASSET_CURRENT, Pending::Exit);
    return;
  }
  StrId error = StrId::STR_LANGUAGE_INSTALL_FAILED;
  if (result == language_cache::Result::Ok) {
    std::strcpy(SETTINGS.languageCode, I18N.getCode());
    SETTINGS.languageCacheGeneration = I18N.getGeneration();
    if (SETTINGS.saveToFile()) {
      pending_ = Pending::Exit;
      return;
    }
    error = StrId::STR_LANGUAGE_SAVE_FAILED;
  }
  LOG_ERR("ASSET", "Activation: %s", language_cache::resultName(result));
  lock.unlock();
  showError(error, Pending::Install);
}
void AssetDownloadActivity::loop() {
  const auto pending = pending_;
  pending_ = Pending::None;
  switch (pending) {
    case Pending::Fetch:
      fetch();
      return;
    case Pending::Download:
      download();
      return;
    case Pending::Overwrite:
      confirmOverwrite();
      return;
    case Pending::Install:
      installSelected();
      return;
    case Pending::Activate:
      activate();
      return;
    case Pending::Exit:
      onGoHome();
      return;
    case Pending::None:
      break;
  }
  {
    RenderLock lock(*this);
    popup_.handleInput(mappedInput, [this] { requestUpdate(); });
  }
}
void AssetDownloadActivity::render(RenderLock&&) {
  renderer.clearScreen();
  TouchHeaderBackButton::drawCompact(renderer, I18N.get(title()));
  if (popup_.isActive()) popup_.processRender(renderer, mappedInput);
}
