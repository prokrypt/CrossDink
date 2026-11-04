#pragma once

#include <AssetCatalog.h>
#include <AssetCompanionIndex.h>
#include <Memory.h>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "network/AssetDownloadService.h"

// A network-boot-only controller. It installs the one requested language end
// to end (download, verify, activate) with the SD language installer. There is
// no catalog list or actions menu.
class AssetDownloadActivity final : public Activity {
 public:
  AssetDownloadActivity(GfxRenderer& renderer, MappedInputManager& input, uint32_t payload);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return busy_; }
  bool requiresFreshBackdrop() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
  static uint32_t bootPayload(uint8_t languageIndex);
  static bool validBootPayload(uint32_t payload);

 private:
  enum class Pending { None, Fetch, Download, Overwrite, Activate, Exit, Install };
  static constexpr AssetCatalog::Kind kind_ = AssetCatalog::Kind::Translation;
  uint8_t wantedIndex_;
  std::unique_ptr<AssetCatalog::Catalog> catalog_;
  AssetDownloadService::Source source_;
  OptionPopup popup_;
  Pending pending_ = Pending::None;
  int selected_ = 0;
  bool busy_ = false;
  bool catalogReady_ = false;
  bool cancelled_ = false;
  bool overwriteApproved_ = false;
  char progress_[64] = {};
  // Fixed install stages (catalog, companions, payload); 0 hides the counter.
  uint8_t step_ = 0;
  char stepLabel_[48] = {};
  char progressLine_[96] = {};
  uint32_t lastProgressAt_ = 0;
  StrId phase_ = StrId::STR_LOADING;
  char url_[AssetDownloadService::MAX_URL] = {};
  AssetDownloadService::Endpoints endpoints_;
  AssetDownloadService::Endpoints catalogEndpoints_;
  bool pollCancel();
  AssetDownloadService::Callbacks callbacks();
  void showBusy(StrId phase = StrId::STR_LOADING);
  OptionPopup::Note busyNote();
  void payloadStarted();
  void progress(size_t received, size_t total);
  void verifying();
  void installSelected();  // discover -> (confirm overwrite) -> download -> activate.
  void showError(StrId error, Pending next = Pending::Exit);
  void confirmOverwrite();
  void fetch();
  void download();
  void activate();
};

static_assert(sizeof(AssetCatalog::Catalog) + sizeof(AssetCatalog::Parser) + sizeof(AssetDownloadActivity) < 16384,
              "Catalog, parser and fixed activity storage exceed the catalog RAM budget");
static_assert(sizeof(AssetCatalog::Catalog) + sizeof(AssetCompanionIndex::Parser) + sizeof(AssetDownloadActivity) <
                  16384,
              "Catalog, companion parser and fixed activity storage exceed the catalog RAM budget");
