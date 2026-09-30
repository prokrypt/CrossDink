#pragma once
#include <Epub.h>
#include <Epub/FootnoteEntry.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuModel.h"
#include "FootnoteLinkTargets.h"
#include "GlobalReadingStats.h"
#include "ManualPageTurnQueue.h"
#include "ReaderProgressSaveDebouncer.h"
#include "SideButtonShortcuts.h"
#include "activities/Activity.h"
#include "activities/reader/TouchReaderPreviewModel.h"
#include "components/HomeCoverThumbs.h"
#include "components/OptionPopup.h"
#if CROSSDINK_APP_CAP_TOUCH
#include "activities/reader/ReaderPinchGesture.h"
#endif

struct ToastRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

class EpubReaderActivity final : public Activity {
 public:
  bool usesFullScreenReaderVerticalSwipes() const override { return true; }

  struct ReaderSettingsSnapshot {
    uint8_t fontFamily = 0;
    uint8_t readerFontPointSize = 14;
    uint8_t lineHeightPercent = 100;
    uint8_t wordSpacing = 0;
    uint8_t orientation = 0;
    uint8_t screenMarginVertical = 5;
    uint8_t screenMarginHorizontal = 5;
    uint8_t publisherPageNumbers = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t embeddedStyle = 1;
    uint8_t hyphenationEnabled = 0;
    uint8_t textAntiAliasing = 1;
    uint8_t imageRendering = 0;
    uint8_t extraParagraphSpacing = 1;
    uint8_t forceParagraphIndents = 0;
    uint8_t focusReadingEnabled = 0;
    uint8_t guideReadingEnabled = 0;
    uint8_t epubRenderMode = 0;
    uint8_t indexingMethod = CrossPointSettings::INDEXING_FULL_SECTION;
    char sdFontFamilyName[64] = "";
  };

  struct BookReaderSettingsData {
    bool hasAutoPageTurnInterval = false;
    uint16_t autoPageTurnSeconds = 0;
    bool hasCustomReaderSettings = false;
    uint32_t readerSettingsOverrideMask = 0;
    bool hasSafeModeOverride = false;
    bool hasRenderModeOverride = false;
    bool hasDictionaryFontOverride = false;
    uint8_t renderMode = 0;
    // Fixed-size per-book state lives inside the already heap-owned reader
    // activity. It avoids a separate string allocation during dictionary use.
    char dictionarySdFontFamilyName[64] = "";
    // Zero follows the reader's current physical point size.
    uint8_t dictionaryFontPointSize = 0;
    ReaderSettingsSnapshot readerSettings;
  };

 private:
  // The on-disk settings record also carries the dictionary family. Keeping it
  // out of the long-lived reader object matters on the C3: this activity is
  // allocated immediately before an EPUB section needs its largest block.
  // Dictionary children load and own the fixed name only while they are open.
  struct ActiveBookReaderSettingsData {
    bool hasAutoPageTurnInterval = false;
    uint16_t autoPageTurnSeconds = 0;
    bool hasCustomReaderSettings = false;
    uint32_t readerSettingsOverrideMask = 0;
    bool hasSafeModeOverride = false;
    bool hasRenderModeOverride = false;
    uint8_t renderMode = 0;
    ReaderSettingsSnapshot readerSettings;

    ActiveBookReaderSettingsData() = default;
    explicit ActiveBookReaderSettingsData(const BookReaderSettingsData& source)
        : hasAutoPageTurnInterval(source.hasAutoPageTurnInterval),
          autoPageTurnSeconds(source.autoPageTurnSeconds),
          hasCustomReaderSettings(source.hasCustomReaderSettings),
          readerSettingsOverrideMask(source.readerSettingsOverrideMask),
          hasSafeModeOverride(source.hasSafeModeOverride),
          hasRenderModeOverride(source.hasRenderModeOverride),
          renderMode(source.renderMode),
          readerSettings(source.readerSettings) {}
  };

  std::shared_ptr<Epub> epub;
  ActiveBookReaderSettingsData initialBookReaderSettings;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int activeSectionFontId = 0;
  uint32_t activeSectionLayoutSignature = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  std::string pendingFootnotePreviewAnchor;
  bool activeFootnotePreview = false;
  int pagesUntilFullRefresh = 0;
  // A Sync Progress return can leave non-reader UI on the panel. This is a
  // one-shot clean base for its first image page; normal image-page cleanup
  // uses pagesUntilFullRefresh independently.
  bool cleanImageBasePending = false;
  // Softfast holds B/W over the Fast base. Book open and the first page after a
  // covering screen (drawer, menus) swing fully so nothing of it ghosts.
  bool smoothFullSwingPending = true;
  uint8_t smoothPagesSinceSwing = 0;  // held Softfast pages since the last full swing
  // The image page whose grayscale pass last reached the panel. Redrawing that
  // same page (an overlay closed) needs no gray-residue cleanup.
  struct GrayImageOnPanel {
    int spine = -1;
    int page = -1;
    int16_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const GrayImageOnPanel& o) const {
      return spine == o.spine && page == o.page && x == o.x && y == o.y && w == o.w && h == o.h;
    }
  };
  GrayImageOnPanel grayImageOnPanel;
  bool skipRecentBookUpdateOnEntry = false;
  int cachedSpineIndex = 0;
  int cachedChapterPageNumber = 0;
  int cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  struct ChapterGroupEstimateCache {
    int currentSpineIndex = -1;
    int firstSpineIndex = -1;
    int lastSpineIndex = -1;
    uint32_t settingsSignature = 0;
    uint64_t knownSiblingImageUnits = 0;
    uint64_t knownSiblingNonImageUnits = 0;
    uint32_t knownSiblingBytes = 0;
    uint32_t unknownSiblingBytes = 0;
    uint64_t precedingKnownImageUnits = 0;
    uint64_t precedingKnownNonImageUnits = 0;
    uint32_t precedingUnknownBytes = 0;
    uint16_t unknownSiblingCount = 0;
    uint16_t precedingUnknownCount = 0;
    bool siblingEstimateUsed = false;
    bool valid = false;
  } chapterGroupEstimate;
  bool pendingRelayoutReposition = false;
  uint16_t cachedPageParagraphIndex = UINT16_MAX;
  uint16_t cachedPageParagraphOffset = 0;
  uint16_t cachedPageParagraphSpan = 0;
  std::atomic<uint8_t> pendingHeapShapeReaderRedrawStages{0};
  static constexpr uint8_t HEAP_SHAPE_REDRAW_CLIP = 1U << 0;
  static constexpr uint8_t HEAP_SHAPE_REDRAW_DICT = 1U << 1;
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  ManualPageTurnQueue pendingManualPageTurns;
  QueuedTurnRenderingState queuedTurnRendering;
  // Any event that makes the current AA pass moot (menu, rotation, sleep, ...)
  // bumps this from the input loop; the render task compares it with the value
  // it saw when its render began. Unlike the turn queue, clearing queued turns
  // can't erase it.
  std::atomic<uint32_t> aaCancelEpoch{0};
  std::atomic<const char*> aaCancelReason{"event"};
  uint32_t aaRenderEpoch = 0;   // render task only
  bool aaCancelLogged = false;  // render task only
  // Set by the render task when it cancels an AA pass, cleared when a render
  // starts; the input loop redraws the page once it is foreground and idle.
  std::atomic<bool> aaRedrawPending{false};
  unsigned long pageShownAtMs = 0UL;
  unsigned long lastRenderCompleteMs = 0UL;
  int idlePrewarmSpine = -1;
  int idlePrewarmPage = -1;
  int idlePrewarmFontId = 0;
  // Next page drawn ahead while idle, into PSRAM (S3). Used by at most the next
  // page-turn render; every renderContents() call clears it.
  HeapByteBuffer prerenderFrameBuffer;
  HeapByteBuffer prerenderSavedFrame;
  std::unique_ptr<Page> prerenderedPage;
  const Section* prerenderedSection = nullptr;
  uint32_t prerenderedKey = 0;
  int prerenderedSpine = -1;
  int prerenderedPageIndex = -1;
  bool prerenderedReady = false;
  const Section* prerenderAttemptSection = nullptr;
  int prerenderAttemptSpine = -1;
  int prerenderAttemptPage = -1;
  bool paceSampleWarmupPending = true;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  uint32_t sessionReadingSeconds = 0;
  uint16_t lastAutoPageTurnIntervalSeconds = 0;
  bool bookHasCustomReaderSettings = false;
  bool bookHasAutoPageTurnInterval = false;
  bool bookHasRenderModeOverride = false;
  bool restoreGlobalReaderSettingsOnExit = false;
  ReaderSettingsSnapshot globalReaderSettingsBeforeBook;
  bool bookReaderSettingsSuspendedForGlobalEdit = false;
  ReaderSettingsSnapshot suspendedBookReaderSettings;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  bool bookStatsEnabled = true;
  bool statsTrackingActive = true;
  bool paceDirty = false;
  bool pendingStatsCommit = false;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  void syncStatsTrackingState();
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  std::optional<uint32_t> pendingReferenceUnitOffset;
  uint32_t pendingReferenceUnitCount = 0;
  bool pendingReferenceUnitsAreCharacters = false;
  std::optional<uint16_t> pendingResolvedReferencePage;
  uint16_t pendingParagraphIndex = UINT16_MAX;
  ReaderDrawerState touchReaderDrawerState{};
#if CROSSDINK_APP_CAP_TOUCH
  std::unique_ptr<TouchReaderPreviewModel> touchReaderPreviewModel;
  bool touchReaderPreviewAllocationAttempted = false;
#endif
  uint16_t pendingClippingIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool automaticPageTurnActive = false;
  // Session-only display toggle. Layout continues to reserve the same status
  // lane, so switching it never changes the EPUB's page breaks.
  bool statusBarVisible = true;
  bool longPressMenuHandled = false;
  bool longPressBackHandled = false;
  bool longPowerButtonHandled = false;
  OptionPopup quickActionsPopup;
  SideButtonShortcuts sideButtonShortcuts;
  bool frontButtonLongPressHandled = false;
  bool touchDictionaryLookupHandled = false;
  int pageLoadRetryCount = 0;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  bool pendingTiltPageTurnFeedback = false;
  bool tiltPageTurnFeedbackEnabled = false;
  bool homeButtonInReaderFeedback = false;
  unsigned long tiltPageTurnFeedbackShowTime = 0UL;
  bool pendingRenderModeToast = false;
  bool renderModeToastShown = false;
  bool pendingSafeModeToast = false;
  bool safeModeToastShown = false;
  uint8_t renderModeToastMode = 0;
  unsigned long renderModeToastShowTime = 0UL;
  std::unique_ptr<uint8_t[]> renderModeToastRegionBuffer;
  size_t renderModeToastRegionBufferSize = 0;
  ToastRect renderModeToastRegion;
  bool renderModeToastRegionSaved = false;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool completionTriggerCrossed = false;
  bool lastAtOrPastCompletionTrigger = false;

  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // The end screen owns these UI resources only while it is visible.
  std::unique_ptr<EndOfBookOptions> endOfBookOptions;
  // First menu press made while the end-of-book menu was still loading (main loop only).
  EndOfBookOptions::MenuKey queuedEndOfBookKey = EndOfBookOptions::MenuKey::None;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
#if CROSSDINK_APP_CAP_TOUCH
  ReaderPinchGesture pinchFontGesture;
  FootnoteLinkTargets currentPageFootnoteTouchTargets{};
#endif
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
  };
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with identical layout parameters to the pages already rendered.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() does not retry every tick.
  bool partialRebuildStartFailed = false;
  // Set when a background extension build aborted for low heap. startBuild() still succeeds in
  // that state -- it is the layout inside buildSomeMore() that runs out of memory -- so without
  // this flag loop() would restart the same doomed build every tick, and skipLoopDelay() would
  // hold the loop at full speed while it did. The reader keeps the pages already laid out; a
  // build is only re-attempted from render() if the reader actually pages past the watermark.
  bool partialRebuildAbortedForLowMemory = false;
  // One-shot guard for the silent restart used before EPUB layout fallback modes. The restart token
  // restores this guard after boot so the resumed attempt can fall through to those modes.
  bool lowMemoryPartialRestartAttempted = false;
  bool backgroundBuildPausedForLowMemory = false;
  // Input should win the next RenderLock race. Keep the incremental parser alive,
  // but do not start another background chunk until the requested render begins.
  std::atomic<bool> backgroundBuildYieldForInput{false};
  // Full-section next-chapter prefetch is speculative. A forward page turn may
  // stop it, but the visible build at the chapter boundary remains full-section.
  std::atomic<bool> silentPrefetchBuildActive{false};
  std::atomic<bool> silentPrefetchCancelRequested{false};
  std::atomic<bool> sectionBuildCancelRequested{false};
  std::atomic<bool> goHomeAfterBuildCancel{false};

  // Last position successfully persisted by saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders.
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;
  ReaderProgressSaveDebouncer progressSaveDebouncer;
  bool progressSaveRequiredAfterRelayout = false;
  // Adapted from Sichroteph/YACP commit 3f3c5fc42e794c021edb9832856ef98c2d2065b9
  // (MIT): retain one render-only strip instead of reallocating it on every
  // grayscale page. Internal-heap storage is released before section/index work;
  // PSRAM storage remains reusable for the reader activity lifetime.
  HeapByteBuffer grayscaleStripScratch;
  size_t grayscaleStripScratchSize = 0;
  bool grayscaleStripScratchInPsram = false;
  // Trigger/memoization concept adapted from Sichroteph/YACP commit
  // 3f3c5fc42e794c021edb9832856ef98c2d2065b9 (MIT).
  int preparedNextSpineIndex = -1;
  uint16_t preparedNextViewportWidth = 0;
  uint16_t preparedNextViewportHeight = 0;

  // Silent next-chapter indexing on the worker core, used when the reader
  // font measures from memory only. The render task keeps showing and turning
  // pages of the current chapter meanwhile; anything else waits for it first.
  struct SilentIndexWorker {
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t done = nullptr;  // given by the task after its build
    std::atomic<bool> finished{false};
    std::atomic<bool> cancel{false};
    int spineIndex = -1;
    uint16_t viewportWidth = 0;
    uint16_t viewportHeight = 0;
    EpubRenderMode renderMode = EpubRenderMode::CrossDinkDefault;
    ReaderRenderSpec spec{};
    bool succeeded = false;
    bool needsRenderLane = false;
    bool laneMissed = false;  // a streamed TTF face: the worker lane cannot serve it
  };
  SilentIndexWorker silentWorker;
  // Start and join happen on the render task and on the loop (font changes).
  SemaphoreHandle_t silentWorkerMutex = nullptr;
  bool silentWorkerOutcomePending = false;  // silentWorkerMutex
  // A chapter the worker could not build (low memory, a streamed TTF face);
  // the render task builds it the old way, with its fallbacks.
  int silentIndexRenderLaneSpine = -1;
  // Reader font the worker lane missed on (a streamed TTF face); both workers
  // skip it until the reader fonts reload. Render task or RenderLock.
  int workerLaneMissFontId = 0;
  static void silentIndexWorkerMain(void* param);
  void runSilentIndexWorker();
  bool canSilentIndexOnWorker(int readerFontId) const;
  bool startSilentIndexWorker(int spineIndex, uint16_t viewportWidth, uint16_t viewportHeight, int readerFontId,
                              EpubRenderMode renderMode);
  void waitSilentIndexWorker(bool cancel);
  bool silentIndexWorkerBusy();
  void applySilentIndexWorkerOutcome();

  // Home's cover thumbs for this book, made once per open on the worker core
  // at idle priority after the page has been still for a moment, so returning
  // Home finds them ready. Loop task only.
  struct HomeThumbWorker {
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t done = nullptr;
    HomeCoverThumbs::Specs specs;
    bool attempted = false;
  };
  HomeThumbWorker homeThumbWorker;
  static constexpr unsigned long HOME_THUMB_IDLE_MS = 3000;
  void maybeStartHomeThumbWorker();
  static void homeThumbWorkerMain(void* param);
  void waitHomeThumbWorker();

  // A page's first-view image caches (ZIP extract + decode, seconds for a big
  // cover) are built on the worker core below the loop task's priority, so the
  // render task never holds RenderLock through them and input stays live. The
  // page shows placeholders, then redraws once the caches exist.
  struct ImageCacheWorker {
    static constexpr uint8_t MAX_IMAGES = 4;
    struct Item {
      std::unique_ptr<ImageBlock> block;  // copy: the page is gone after its render
      int16_t x = 0;
      int16_t y = 0;
      ImageBlock::CacheBuild result = ImageBlock::CacheBuild::Cancelled;
    };
    Item items[MAX_IMAGES];
    uint8_t count = 0;
    int spine = -1;  // the page the job was started for
    int pageIndex = -1;
    std::unique_ptr<GfxRenderer> renderer;  // offscreen; kept for the session
    HeapByteBuffer frame;                   // PSRAM scratch the decoder draws into
    TaskHandle_t task = nullptr;            // render task (and onExit) only
    SemaphoreHandle_t done = nullptr;
    std::atomic<bool> cancel{false};
    std::atomic<bool> redraw{false};  // worker -> loop: caches ready, draw the page again
  };
  ImageCacheWorker imageCacheWorker;
  // Render task: true when the page's missing image caches are (still) being
  // built in the background and the page should draw placeholders for now.
  bool startImageCacheWorker(const Page& page, int marginLeft, int marginTop);
  static void imageCacheWorkerMain(void* param);
  // Joins a finished job (all of it when cancel), applying its failures.
  // False while it still runs. Render task or onExit.
  bool joinImageCacheWorker(bool cancel);

  // Draw-ahead on the worker core: right after a page is shown, the next page
  // is drawn into prerenderFrameBuffer with an offscreen renderer and its own
  // glyph decompressor, while the render task stays free. Used when the page's
  // fonts are in memory and no clippings need highlighting; otherwise the idle
  // prerenderNextPage() path runs as before.
  struct DrawAheadWorker {
    std::unique_ptr<GfxRenderer> renderer;
    std::unique_ptr<FontDecompressor> decompressor;
    std::unique_ptr<FontCacheManager> fontCache;
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t done = nullptr;
    std::unique_ptr<Page> page;
    const Section* section = nullptr;
    int spine = -1;
    int pageIndex = -1;
    int fontId = 0;
    int marginTop = 0;
    int marginLeft = 0;
    int contentBottom = 0;
    uint32_t key = 0;
    bool foregroundBlack = true;
    uint8_t background = 0xFF;
    bool drawn = false;
    bool laneMissed = false;
    bool pending = false;  // a finished draw not yet taken (drawAheadMutex)
  };
  DrawAheadWorker drawAhead;
  SemaphoreHandle_t drawAheadMutex = nullptr;
  static void drawAheadWorkerMain(void* param);
  void runDrawAhead();
  void startDrawAhead(int fontId, int marginTop, int marginLeft, int contentBottom, uint32_t layoutKey);
  // publish: render task only (RenderLock), moves a finished draw into the
  // prerendered* fields; otherwise the draw is discarded.
  void waitDrawAhead(bool publish);

  bool renderContents(std::unique_ptr<Page> page, int fontId, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft, bool updatePanel,
                      bool usePrerenderedFrame = false);
  bool ensureGrayscaleStripScratch();
  void releaseGrayscaleStripScratch(bool force = false);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  void refreshChapterGroupEstimate(uint16_t viewportWidth, uint16_t viewportHeight);
  bool resolveChapterGroupPageProgress(int& currentPage, int& pageCount, float& chapterProgress,
                                       bool& pageCountEstimated) const;
  bool isTocChapterDestination(int targetSpineIndex, const std::string& anchor) const;
  bool shouldUseFootnotePreview(int targetSpineIndex, const std::string& anchor) const;
  std::string footnotePreviewCacheSuffix(EpubRenderMode renderMode, const std::string& anchor) const;
  void clearFootnotePreviewState();
  void silentIndexNextChapterIfNeeded(uint16_t viewportWidth, uint16_t viewportHeight);
  void cancelSilentPrefetchForInput();
  bool restoreCurrentPageBufferAfterSilentIndex();
  // Larger batches are reserved for non-interactive work such as sleep-page preparation.
  static constexpr int BUILD_PAGES_PER_CHUNK = 8;
  // Interactive builds stop as soon as the requested page is ready and give the
  // main loop a chance to observe input between pages.
  static constexpr int INTERACTIVE_BUILD_PAGES_PER_CHUNK = 1;
  // Ticking one page at a time (checked against RenderLock::peek() and the input-yield flag
  // before every tick) keeps the background build responsive. Incremental limits the build to
  // a small lookahead window, while IncreMENTAL keeps working to completion.
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 1;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  // Show the indexing popup when an initial build must lay out more than this many pages up front
  // (a deep resume/jump into a not-yet-built section), so it isn't a silent wait. Kept independent
  // of the background build so ordinary landings stay popup-free.
  static constexpr int BUILD_POPUP_PAGE_THRESHOLD = 20;
  // Also show the popup when first building a spine larger than this (uncompressed bytes): its
  // whole HTML must be inflated before page 1 can lay out (the giant single-spine case), which is
  // a multi-second wait. Normal chapters are well under this and stay popup-free.
  static constexpr size_t BUILD_POPUP_BYTE_THRESHOLD = 96 * 1024;
  // If a build predicted to be fast still has not produced the requested page within this
  // window, show the popup while the blocking build continues.
  static constexpr unsigned long BUILD_POPUP_DEADLINE_MS = 1000;
  // Only true during the blocking build-to-target phase. The parser retains the callback during
  // background indexing, so this guard prevents it from drawing over an already-visible page.
  bool buildPopupPending = false;
  void showBuildPopup();
  // Remap the cached reading position once the saved paragraph and prior readable watermark are rebuilt
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  bool isRelayoutCatchUpComplete() const;
  bool applyDeferredReposition();
  // Saves are suppressed while a footnote preview is on screen so the preview's own
  // position cannot overwrite the reader's. Set allowDuringFootnotePreview for the
  // deliberate on-exit save of the pre-footnote origin, which is the position the
  // suppression exists to protect.
  bool saveProgress(int spineIndex, int currentPage, int pageCount, bool allowDuringFootnotePreview = false);
  bool queueProgressSave(int spineIndex, int currentPage, int pageCount, bool forceSave = false);
  bool flushQueuedProgress();
  bool saveFootnoteOriginProgress();
  // Saves the position now, the way onExit() does (the link origin while in a
  // footnote). Used before handing off to anything that can silently restart
  // the device, which reboots without running onExit().
  void saveProgressBeforeRestart();
  void cacheCurrentSectionPosition();
  void pauseReadingPaceTimer(const char* reason = "unknown");
  void resumeReadingPaceTimer(const char* reason = "unknown");
  void armReadingPaceWarmup(const char* reason = "unknown");
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  void recordForwardPagePaceSample(uint32_t seconds, const char* source);
  bool getSessionAveragePaceSeconds(uint16_t& avgSeconds) const;
  void recoverStoredPaceFromSession(const char* reason = "unknown");
  bool getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source, uint16_t& sampleCount) const;
  bool estimateRemainingTimeLeftPages(bool bookEstimate, float& remainingPages) const;
  bool estimateProgressTimeLeftSeconds(uint32_t& seconds) const;
  bool estimateTimeLeftSeconds(bool bookEstimate, uint32_t& seconds) const;
  bool formatTimeLeftLabel(char* buf, size_t len, bool bookEstimate) const;
  void refreshCachedTimeLeftEstimate();
  void applyBookStatsEditsFromDisk();
  void handleBookStatsReturn(bool returnToReaderMenu);
  void resetCurrentBookStatsAfterDelete();
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false, bool returnToReaderMenu = false);
  void startClipSelection(const DictionaryClippingRequest* dictionaryRequest = nullptr,
                          bool ignoreInitialBackRelease = false);
  void resetReadingPaceData();
  void captureGlobalReaderSettings();
  void restoreGlobalReaderSettings();
  void loadBookReaderSettings();
  void saveCurrentBookReaderSettings();
  void saveDictionaryFontForBook(const char* familyName, uint8_t pointSize);
  void persistReaderSdFontSettings();
  bool saveGlobalSettingsPreservingBookOverrides();
  bool beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();
  static void saveReaderOptionsForBook(void* ctx);
  static void saveDictionaryFontForBookReader(void* ctx, const char* familyName, uint8_t pointSize);
  static void persistReaderSdFontSettingsForBook(void* ctx);
  static void saveGlobalSettingsForBookReader(void* ctx);
  static void beginGlobalSettingsEditForBookReader(void* ctx);
  static void endGlobalSettingsEditForBookReader(void* ctx);
  // Jump to a percentage of the book (0.0-100.0, two decimals meaningful), mapping it to spine and page.
  void jumpToPercent(float percent);
  void jumpToStablePage(uint32_t page);
  void reindexCurrentSection();
  void prepareCurrentSectionForRelayout();
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action,
                                bool dictionaryLookupFramebufferContainsPage = true,
                                QuickLockTrigger quickLockTrigger = QuickLockTrigger::LongMenu);
  void openQuickActionsPopup();
  void executeFootnoteQuickAction(bool suppressInitialPowerRelease = false);
  void openFootnoteSelect(bool returnToReaderMenu);
#if CROSSDINK_APP_CAP_TOUCH
  bool handlePinchFontResize();
  void resetPinchFontGesture();
  void buildFootnoteTouchTargets(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft);
  bool handleTouchFootnoteLink(int touchX, int touchY);
#endif
  void suppressPowerShortcutRelease();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void handleClippingJump(const ClippingJumpResult& clipping);
  bool handleTouchDictionaryLookup();
  void openWordSelect(bool framebufferContainsPage, int initialTouchX = -1, int initialTouchY = -1,
                      bool autoLookupInitialWord = false);
  std::unique_ptr<Page> reloadDictionaryLookupPage(int pageOffset = 0);
  static std::unique_ptr<Page> reloadDictionaryLookupPageCallback(void* context, int pageOffset);
  void onReaderMenuConfirm(EpubReaderMenuAction action, bool returnToReaderMenu = false,
                           const PendingOverlayResume* replacementResume = nullptr);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void applyOrientation(uint8_t orientation);
  void requestManualPageTurn(bool isForwardTurn, const char* source);
  bool drainPendingManualPageTurn();
  // Also cancels a running AA pass; `aaReason` names the event in the log.
  void clearPendingManualPageTurns(bool requestRecoveryRedraw = true, const char* aaReason = "turns-cleared");
  void cancelGrayscalePass(const char* reason);
  // Render task: true when the AA pass should stop at `checkpoint` (a queued
  // turn or a cancel event since the render began). Flags a recovery redraw.
  bool grayscalePassCancelled(const char* checkpoint);
  void finishManualPageTurnBrakeIfReady();
  void cancelSilentNextChapterPrefetchForForwardTurn();
  bool isAtBookStart() const;
  void pageTurn(bool isForwardTurn, const char* source = "unknown");
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  bool shouldQueueCompletionPromptOnChapterExit() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);
  void showTiltPageTurnFeedback(bool enabled);
  // Shared dismissal rule for the transient bookmark/completed/tilt confirmations.
  bool transientFeedbackDismissed(unsigned long showTimeMs) const;
  void toggleHomeButtonInReader();
  void showRenderModeToast(uint8_t renderMode);
  void showSafeModeToast();
  bool storeRenderModeToastRegion(const char* msg);
  void drawRenderModeToastBuffer(const char* msg);
  bool restoreRenderModeToastRegion();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false, bool preferFootnotePreview = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<Epub> epub,
                              const BookReaderSettingsData& readerSettings, int initialRefreshCountdown,
                              bool cleanImageBaseOnEntry = false, bool skipRecentBookUpdateOnEntry = false)
      : Activity("EpubReader", renderer, mappedInput),
        epub(std::move(epub)),
        initialBookReaderSettings(readerSettings),
        pagesUntilFullRefresh(initialRefreshCountdown),
        cleanImageBasePending(cleanImageBaseOnEntry),
        skipRecentBookUpdateOnEntry(skipRecentBookUpdateOnEntry) {}
  void onEnter() override;
  void onExit() override;
  void onCovered() override {
    smoothFullSwingPending = true;
    // Menus over a Softfast page get the longer repaint (driver keys it on smooth gray).
    renderer.setSmoothGray(SETTINGS.textAntiAliasing == CrossPointSettings::TEXT_AA_SMOOTH);
    waitSilentIndexWorker(/*cancel=*/true);
    waitDrawAhead(/*publish=*/false);
  }
  void loop() override;
  void render(RenderLock&& lock) override;
  bool handleTwoFingerSwipeAction(CrossPointSettings::TWO_FINGER_SWIPE_ACTION action) override;
  bool handleTwoFingerRotation(bool clockwise) override;
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = -1;
    cleanImageBasePending = true;
    return true;
  }
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  // Hold the loop hot only while the build has work this loop would do: a kept-alive
  // build sitting outside the lookahead window is dormant, and reporting it here would
  // pin the CPU at full clock (no power saving, yield-only loop) for the whole read.
  // Mirrors the tick condition in loop(): catch-up phase, or watermark inside the window.
  // Caller must own RenderLock: render() can replace or finalize section.
  bool sectionBuildWantsTick() const {
    if (!section || !section->isBuilding()) {
      return false;
    }
    if (SETTINGS.indexingMethod != CrossPointSettings::INDEXING_INCREMENTAL) {
      return true;
    }
    return !section->activeBuildHasCaughtReadablePages() ||
           static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD;
  }
  bool backgroundSectionBuildHasHeap();
  void idlePrewarmNextPage();
  void prerenderNextPage();
  void clearPrerenderedPage();
  std::unique_ptr<Page> takePrerenderedPage(uint32_t layoutKey);
  void prewarmNextPageFonts(const char* when);
  bool skipLoopDelay() override {
    return sectionBuildWantsTick() && !backgroundBuildPausedForLowMemory &&
           !backgroundBuildYieldForInput.load(std::memory_order_relaxed);
  }
  bool isReaderActivity() const override { return true; }
  bool isEpubReaderActivity() const override { return true; }
  void onInputLockChanged(bool locked) override;
  void cancelOptionalRenderWork(const char* reason) override { cancelGrayscalePass(reason); }
  void onUserInput() override;
  bool handleQuickLockUnlock(QuickLockTrigger trigger) override;
  bool canSnapshotForSleepOverlay() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return quickActionsPopup.isActive(); }
  bool blocksGlobalInput() const override { return quickActionsPopup.isActive(); }
  bool handleShortcutAction(CrossPointSettings::SHORT_PWRBTN action) override;
  bool openReaderSettingsMenu() override {
    if (!epub) {
      return false;
    }
    openReaderMenu();
    return true;
  }
  bool handleShortcutAction(uint8_t action) override;
  std::string getCurrentBookPath() const override { return epub ? epub->getPath() : std::string{}; }
  std::string getCurrentBookTitle() const override { return epub ? epub->getTitle() : std::string{}; }
  bool getFrontlightPanelBookDetails(FrontlightPanelBookDetails& details) override;
  std::unique_ptr<Activity> createFrontlightReadingStatsActivity() override;
  void onFrontlightPanelOpened() override;
  void onFrontlightPanelClosed() override;
  void onBackdropRenderedForOverlay() override { pageShownAtMs = 0UL; }
  void persistGlobalSettings() override { saveGlobalSettingsPreservingBookOverrides(); }
  bool onFrontlightGlobalSettingsOpened() override { return beginGlobalSettingsEdit(); }
  void onFrontlightGlobalSettingsClosed() override { endGlobalSettingsEdit(); }
  bool handleFrontlightPanelResult(const FrontlightPanelResult& result) override;
  bool handleExternalReaderMenuAction(uint8_t action) override;
  bool restorePendingOverlay(const PendingOverlayResume& resume) override;
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  static BookReaderSettingsData readBookReaderSettings(const Epub& epub);
  // Whether the reader lays this book out in landscape: its own orientation
  // override, else the global setting. Picks the section cache that position
  // sync reads outside the reader.
  static bool bookUsesLandscapeLayout(const Epub& epub);
  static uint8_t loadBookRenderMode(const std::string& filePath);
  static bool saveBookRenderMode(const std::string& filePath, uint8_t renderMode);
  static bool resetBookReaderSettings(const std::string& filePath);
  ScreenshotInfo getScreenshotInfo() const override;
};
