#pragma once

#include <AppCapabilities.h>

#if CROSSDINK_APP_CAP_TOUCH

#include <Epub.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "EpubReaderMenuModel.h"
#include "ReaderOptionsActivity.h"
#include "TouchReaderPreviewModel.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"
#if CROSSDINK_SCALABLE_FONTS
#include "TtfRenderProfileStore.h"
#endif

class EpubReaderTouchMenuActivity final : public Activity {
 public:
  explicit EpubReaderTouchMenuActivity(
      GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
      const TouchReaderPreviewModel* previewModel, float bookProgressPercent, bool hasFootnotes, bool hasDictionary,
      bool hasBookmarks, bool hasClippings, bool isCurrentPageBookmarked, bool isBookCompleted,
      bool showReadingPaceReset, uint32_t stableCurrentPage, uint32_t stablePageCount,
      uint16_t autoPageTurnIntervalSeconds, bool automaticPageTurnActive,
      ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback, void* saveReaderSettingsContext,
      ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback, void* saveGlobalSettingsContext,
      ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback,
      void* beginGlobalSettingsEditContext,
      ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback,
      void* endGlobalSettingsEditContext, const char* dictionaryFontFamilyName, uint8_t dictionaryFontPointSize,
      bool hasDictionaryFontOverride,
      ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback,
      void* dictionaryFontChangedContext, const ReaderDrawerState& initialState = {});

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowFrontlightPanelGesture() const override { return false; }
  // A dirty preview can rebuild the page area itself; after a TTF ID change,
  // an unavailable text snapshot is replaced with a safe blank background.
  bool requiresFreshBackdrop() const override {
    return readerDrawerNeedsExternalBackdrop(previewDirty, previewModel && previewModel->valid(),
                                             previewFontMetricsChanged);
  }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

  // Route the touch-screen edge swipe through loop() so it can go Home while
  // preserving the capacitive Home key's existing drawer-back behavior.
  bool allowGlobalHomeSwipeGesture() const override { return false; }
  bool handleHomeGesture() override;

 private:
  using RowId = ReaderDrawerCatalogItem;

  using UiApp = freeink::ui::FreeInkApp<48, 11>;
  static constexpr freeink::ui::ActionId ACTION_ROW = 1;
  static constexpr freeink::ui::ActionId ACTION_TAB = 2;
  static constexpr freeink::ui::ActionId ACTION_DISMISS = 3;
  static constexpr freeink::ui::ActionId ACTION_BACK = 4;
  static constexpr freeink::ui::ActionId ACTION_SLIDER = 5;
  static constexpr freeink::ui::ActionId ACTION_STEP = 6;
  static constexpr freeink::ui::ActionId ACTION_CONFIRM = 7;
  // 8 and 9 are taken by the dual-slider panes' second control (ACTION_SLIDER + 3,
  // ACTION_STEP + 3); the keypad grid's digit/dot/OK keys share one action.
  static constexpr freeink::ui::ActionId ACTION_KEYPAD_KEY = 10;
  static constexpr freeink::ui::ActionId ACTION_KEYPAD_BACKSPACE = 11;
  static constexpr size_t WINDOW_SIZE = 20;

  std::shared_ptr<Epub> epub;
  const TouchReaderPreviewModel* previewModel = nullptr;
  // Centipercent (0-10000, hundredths of a percent) so the keypad can type a decimal
  // destination; 1.00% is 100 here.
  int percent = 0;
  uint32_t stablePage = 0;
  uint32_t stablePageCount = 0;
  // The value each pane opened with (set once in the constructor), restored by
  // resetKeypadEntry() so backspacing everything or leaving and reopening the pane
  // shows the book's actual position again rather than an abandoned typed value.
  const int percentSeed = 0;
  const uint32_t stablePageSeed = 0;
  // Numeric keypad entry for the Percent/StablePage panes (typed digits, not the old
  // slider), matching EpubReaderPercentSelectionActivity's non-touch keypad.
  char entryText[8] = {0};
  uint8_t entryLen = 0;
  bool hasFootnotes = false;
  bool hasDictionary = false;
  bool hasBookmarks = false;
  bool hasClippings = false;
  bool isCurrentPageBookmarked = false;
  bool isBookCompleted = false;
  bool showReadingPaceReset = false;
  bool settingsChanged = false;
  bool didChangeSettings = false;
  bool previewDirty = false;
  bool previewFontMetricsChanged = false;
  bool fontPreviewLoading = false;
  int16_t previousDrawerTop = -1;
  bool draggingSlider = false;
  bool sliderTapPending = false;
  bool buttonFocusActive = false;
  bool automaticPageTurnActive = false;
  uint16_t autoPageTurnIntervalSeconds = READER_AUTO_PAGE_TURN_MIN_SECONDS;

  ReaderDrawerState state{};
  ReaderSettingsDraft draft{};
  const ReaderSettingsDraft sourceSettings;
  ReaderSettingsChangeMask changeMask = ReaderSettingsChangeMask::None;
  std::array<std::vector<RowId>, READER_DRAWER_TAB_COUNT> rootRows;
  std::vector<RowId> paneRows;
  std::vector<std::string> fontLabels;
  std::vector<uint8_t> fontSettingIndexes;
  std::vector<std::string> enumOptionLabels;
  std::vector<uint8_t> enumOptionValues;
  RowId enumOptionRow = RowId::FontSize;
  StrId enumOptionTitle = StrId::STR_NONE_OPT;
  ReaderDrawerPane enumOptionReturnPane = ReaderDrawerPane::Root;
  int16_t enumOptionSelectedIndex = 0;
  int16_t previewedEnumOptionIndex = -1;
  std::vector<std::string> dictionaryLabels;
  std::vector<std::string> dictionaryPaths;
  std::string bookDictionaryPath;
  std::array<std::string, WINDOW_SIZE> labelWindow{};
  std::array<freeink::ui::ListItem, WINDOW_SIZE> itemWindow{};
  // Owned here rather than as a render-local array, matching itemWindow/labelWindow
  // above: keeps the render task's stack frame small.
  std::array<freeink::ui::KeyGridKey, 12> keypadKeys{};

  ReaderOptionsActivity::SaveSettingsCallback saveReaderSettingsCallback = nullptr;
  void* saveReaderSettingsContext = nullptr;
  ReaderOptionsActivity::SaveGlobalSettingsCallback saveGlobalSettingsCallback = nullptr;
  void* saveGlobalSettingsContext = nullptr;
  ReaderOptionsActivity::GlobalSettingsEditCallback beginGlobalSettingsEditCallback = nullptr;
  void* beginGlobalSettingsEditContext = nullptr;
  ReaderOptionsActivity::GlobalSettingsEditCallback endGlobalSettingsEditCallback = nullptr;
  void* endGlobalSettingsEditContext = nullptr;
  char dictionaryFontFamilyName[64] = {};
  uint8_t dictionaryFontPointSize = 0;
  bool hasDictionaryFontOverride = false;
  ReaderOptionsActivity::DictionaryFontChangedCallback dictionaryFontChangedCallback = nullptr;
  void* dictionaryFontChangedContext = nullptr;
  ButtonNavigator buttonNavigator;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  freeink::ui::Rect drawerHandleRect{};

  static void drawerScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onTabEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onDismissEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onBackEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onSliderEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onStepEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onConfirmEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onKeypadKeyEvent(const freeink::ui::ActionEvent& event, void* user);
  static void onKeypadBackspaceEvent(const freeink::ui::ActionEvent& event, void* user);

  void buildDrawer(UiApp::ScreenType& screen);
  void buildTabBar(UiApp::ScreenType& screen, freeink::ui::Rect rect, bool drawBottomRule);
  void buildPaneHeader(UiApp::ScreenType& screen);
  void buildRootRows(UiApp::ScreenType& screen);
  void buildSimplePane(UiApp::ScreenType& screen);
  void buildSpacingPane(UiApp::ScreenType& screen);
  void buildMarginsPane(UiApp::ScreenType& screen);
  void buildPercentPane(UiApp::ScreenType& screen);
  void buildStablePagePane(UiApp::ScreenType& screen);
  // Shared by both panes: a readout with a backspace icon, and a 4x3 grid (1-9 / 0,
  // ., OK; Percent only enables "."). Unlike every other pane, there is no separate
  // Confirm button here - OK lives in the grid instead, since the sheet has no room
  // for both. The pre-existing hardware-Confirm handling in loop() still applies.
  void buildDrawerKeypad(UiApp::ScreenType& screen, bool allowDecimal, const char* value);
  void buildAutoPageTurnPane(UiApp::ScreenType& screen);
  void buildConfirmButton(UiApp::ScreenType& screen);
  void buildDictionaryPane(UiApp::ScreenType& screen);
  void buildFontFamilyPane(UiApp::ScreenType& screen);
  void buildEnumOptionsPane(UiApp::ScreenType& screen);
  void buildTtfRenderingPane(UiApp::ScreenType& screen);

  const std::vector<RowId>& activeRows() const;
  int activeTopIndex() const;
  void activateRow(RowId row);
  void activateListIndex(int index);
  void openPane(ReaderDrawerPane pane);
  void closePane();
  void changeTab(ReaderDrawerTab tab);
  void closeAndReturn(bool cancelled, EpubReaderMenuAction action = EpubReaderMenuAction::GO_HOME,
                      bool reopenDrawer = true);
  void commitSettings();
  static ReaderSettingsDraft captureSettings();
  static void applySettings(const ReaderSettingsDraft& settings);
  void markSettingChanged(ReaderSettingsChangeMask mask);
  void moveSelection(bool forward, bool page);
  void scrollBy(int delta);
  void showEnumOptions(RowId row);
  void openEnumOptions(RowId row, StrId title, std::vector<std::string> labels, std::vector<uint8_t> values,
                       int selectedIndex);
  void selectEnumOption(int index);
  void completePercentSelection();
  void completeStablePageSelection();
  void completeAutoPageTurnSelection();
  void notifyDictionaryFontChanged();
  void toggleSetting(RowId row);
  void adjustActiveSlider(int delta);
  void setActiveSliderPermille(int16_t permille);
  void appendKeypadDigit(char digit);
  void appendKeypadDecimalPoint();
  void backspaceKeypadEntry();
  void resetKeypadEntry();
  // Parses entryText (if any digits were typed) into percent/stablePage so both the
  // grid's OK key and the pane's hardware-Confirm handling in loop() always read the
  // latest typed value.
  void syncKeypadValue();
  int16_t drawerHeight() const;
  bool renderPreview(int& previewFontId);
  void renderPreviewWithAntiAliasing(int previewFontId);
  void renderPreviewContents(const ReaderSettingsDraft& previewSettings, int previewFontId);
  void renderPreviewText(const ReaderSettingsDraft& previewSettings, int previewFontId);
  void discoverFonts();
  void refreshTtfRenderingRow();
#if CROSSDINK_SCALABLE_FONTS
  TtfRenderProfile ttfRenderProfile{};
  TtfRenderProfile initialTtfRenderProfile{};
  bool ttfRenderingChanged = false;

  void rebuildTtfRenderingRows();
  void showTtfRenderingOptions(RowId row);
  void saveTtfRenderingProfile();
  void finishTtfRenderingEdit();
#endif
  void discoverDictionaries();
  bool saveBookDictionary(const std::string& path);
  const char* rowLabel(RowId row) const;
  const char* rowValue(RowId row, char* buffer, size_t bufferSize) const;
  static bool rowIsToggle(RowId row);
  bool rowShowsNavigationCaret(RowId row) const;
  bool rowToggleValue(RowId row) const;
  const char* paneTitle() const;
};

#endif
