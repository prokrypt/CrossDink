#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

#include <atomic>

class HalDisplay {
 public:
  using GrayscaleMode = freeink::GrayscaleMode;
  using GrayscaleCapabilities = freeink::GrayscaleCapabilities;
  GrayscaleCapabilities grayscaleCapabilities(GrayscaleMode mode = GrayscaleMode::Overlay) const;
  // Constructor with pin configuration
  HalDisplay();

  // Destructor
  ~HalDisplay();

  // Refresh modes
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  bool displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen = false);

  // Pass seamless=true on any path where the panel already shows the
  // content it should after begin() returns (silent reboot's popup,
  // sleep-wake with a restored buffer). Skips the wakeup-gated
  // requestResync() and defuses the SDK's X3 _x3InitialFullSyncsRemaining
  // counter; otherwise the first two paints get promoted to FULL
  // (~770ms each on X3).
  void begin(bool seamless = false);
  // After a silent restart: the panel still shows `frame`, so the driver can
  // use it as the previous frame and make the first paint Fast. Call after
  // begin(). False when the panel driver cannot use it.
  bool seedDisplayedFrame(const uint8_t* frame);
  bool grayOnPanel() const { return einkDisplay.grayOnPanel(); }

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = EInkDisplay::DISPLAY_WIDTH;
  static constexpr uint16_t DISPLAY_HEIGHT = EInkDisplay::DISPLAY_HEIGHT;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;

  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  // Persistently invert panel output while leaving framebuffer drawing in its
  // normal logical colors.
  void setInverted(bool inverted);
  // Non-blocking refresh (shadow-free): starts the panel waveform and returns
  // while the panel refreshes on its own. The framebuffer must stay untouched
  // until waitRefreshComplete(), and the caller must rebuild the differential
  // baseline before the next differential update (the tiled grayscale cleanup
  // does). Panels without deferral fall back to a blocking refresh.
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH);
  // Block until a pending deferred refresh completes (no-op when none is).
  void waitRefreshComplete();
  // Non-blocking refresh for menus: starts the waveform and returns. A copy of
  // the frame keeps the differential baseline, so the framebuffer may be
  // redrawn at once. Every other display call finishes the refresh first.
  // Night Mode, X3 and panels without deferral take the blocking path.
  void displayBufferDeferred(RefreshMode mode = RefreshMode::FAST_REFRESH);
  // True while a started refresh has not been finished with waitRefreshComplete().
  bool isRefreshPending() const;
  // True while that refresh's waveform is still running on the panel.
  bool isRefreshBusy();
  // True when displayBufferAsync() genuinely overlaps (panel driver defers);
  // false where it falls back to a blocking refresh.
  bool supportsAsyncRefresh() const;
  // True when the normal async refresh is equivalent to the panel's grayscale
  // base pass. X3 has a dedicated grayscale waveform and must stay blocking.
  bool supportsAsyncGrayscaleBase() const;
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  bool isInverted() const;

  // Power management
  void deepSleep();
  // Booster off between refreshes on idle screens; false when nothing changed.
  bool powerOffIdle();
  // Booster on ahead of the next refresh (touch-down); false when nothing changed.
  bool powerOnIdle();
  // Trial: light-sleep through refresh busy-waits (HalPowerManager::setRefreshLightSleep).
  void setRefreshLightSleep(bool allowed);

  // Access to frame buffer
  uint8_t* getFrameBuffer() const;

  // Lend the framebuffer's ~48 KB STORAGE to a memory-hungry phase (chapter
  // builds) without freeing it: the allocation never moves, so repeated loans
  // cannot fragment the heap (free+realloc measurably did). No display calls
  // between lend and return; the panel keeps its last refreshed image. The
  // buffer comes back white — redraw fully. Returns nullptr if already lent.
  uint8_t* lendFrameBufferStorage(uint32_t* sizeOut);
  void returnFrameBufferStorage();

  // X3 grayscale preconditioning (OEM "AA-pre-BW(mid)" settle pass), windowed
  // to the gray region in physical panel coordinates (no-arg = full frame).
  // Call after the BW base frame is displayed and before the grayscale planes
  // are written; no-op on X4. See EInkDisplay::preconditionGrayscale.
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);

  // Display the framebuffer as the base frame for a grayscale overlay that
  // follows. X3 uses the OEM differential base waveform ("AA-pre-BW(mid)");
  // other panels display normally with `fallback` mode (previous behavior).
  // Forces the same X3 resync that displayBuffer(HALF)/displayBuffer(FULL) do
  // whenever `fallback` isn't FAST_REFRESH, so a HALF/FULL caller gets a clean
  // base instead of ghosting through the differential waveform.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);
  // Deferred base (X4 Pro): true while the base waveform still runs; finish it
  // with waitRefreshComplete() and keep the framebuffer untouched until then.
  bool displayGrayscaleBaseAsync(RefreshMode fallback = FAST_REFRESH);
  bool supportsDeferredGrayscaleBase() const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);
  // millis() when the running refresh's full-screen swing shows (UC8179: from
  // the driver, per waveform; it may lie ahead), 0 while none runs. The main
  // loop dims the frontlight around it.
  uint32_t flashStartedMs() const;
  // A refresh that may flash is running (the loop ticks fast meanwhile).
  bool flashMarked() const { return flashStart.load(std::memory_order_relaxed) != 0; }
  // When that refresh is expected to end (UC8179), 0 when unknown.
  uint32_t flashEndsMs() const;
  // Goodies > Battery & stats: refreshes by kind, kept in RTC memory across deep
  // sleep and restarts (all zero in other builds). n[FULL/HALF/FAST_REFRESH],
  // then gray passes, then refreshes that flash (as marked for the flash duck).
  enum { GRAY_PASSES = 3, FLASHING = 4 };
  struct RefreshCounts {
    uint32_t magic;
    uint32_t n[5];
  };
  static RefreshCounts& refreshCounts();

  // Tiled grayscale: stream one band of a plane (lsbPlane selects LSB/MSB RAM)
  // straight to the controller; supportsStripGrayscale() gates the path. See
  // EInkDisplay::writeGrayscalePlaneStrip.
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  // Firmware policy for the reader's extra white-image refresh.
  bool shouldSkipImageBlanking() const;
  // UC8179 Smooth text AA (short balanced gray nudge); other panels ignore it.
  void setSmoothGray(bool smooth);
  // UC8179: Fast diffs against the true on-screen frame (and the driver runs its
  // clean waveform itself when that frame is unknown), so a screen change needs
  // no separate cleanup refresh.
  bool fastTracksPanel() const;
  bool supportsStripGrayscale() const;

  // Runtime geometry passthrough
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  void markFlash(bool flashes);
  static void count(int kind);
  // Marks a synchronous refresh (if it flashes) and clears the mark when it returns.
  struct FlashScope {
    HalDisplay& d;
    FlashScope(HalDisplay& display, const bool flashes) : d(display) { d.markFlash(flashes); }
    ~FlashScope() { d.flashStart.store(0, std::memory_order_relaxed); }
  };
  std::atomic<uint32_t> flashStart{0};
  bool smoothGray = false;
  EInkDisplay einkDisplay;
};

extern HalDisplay display;
