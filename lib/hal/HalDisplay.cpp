#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <PerfLog.h>

#include "HalSpiBus.h"

// Global HalDisplay instance
HalDisplay display;

#if CROSSDINK_GOODIES
namespace {
RTC_NOINIT_ATTR HalDisplay::RefreshCounts rtcRefreshCounts;  // survives deep sleep; random after power loss
constexpr uint32_t REFRESH_COUNTS_MAGIC = 0x52465243;       // "RFRC"
}  // namespace

HalDisplay::RefreshCounts& HalDisplay::refreshCounts() {
  if (rtcRefreshCounts.magic != REFRESH_COUNTS_MAGIC) rtcRefreshCounts = {REFRESH_COUNTS_MAGIC, {}};
  return rtcRefreshCounts;
}

void HalDisplay::count(const int kind) { refreshCounts().n[kind]++; }
#else
HalDisplay::RefreshCounts& HalDisplay::refreshCounts() {
  static RefreshCounts none{};
  return none;
}

void HalDisplay::count(int) {}
#endif

// Builds with a PSRAM noinit segment (S3) keep the gray planes for screenshots.
#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY && !defined(SIMULATOR)
#define GRAY_SHOT 1
#else
#define GRAY_SHOT 0
#endif

namespace {
#if GRAY_SHOT
// Screenshots: copies of the last gray pass's planes, so screenshots can show 4
// levels. PSRAM statics, never read by rendering.
constexpr uint32_t SHOT_PLANE_MAX = 48000;  // largest current panel
EXT_RAM_NOINIT_ATTR uint8_t shotLsb[SHOT_PLANE_MAX];
EXT_RAM_NOINIT_ATTR uint8_t shotMsb[SHOT_PLANE_MAX];
bool shotAbsolute = false;
bool shotShown = false;  // planes match the panel: a gray pass ran, no B/W refresh since

void shotRefresh(const bool absolute) {
  shotShown = false;
  shotAbsolute = absolute;
}
void shotPlane(const bool lsb, const uint8_t* src, const uint32_t offset, const uint32_t len) {
  if (src && offset + len <= SHOT_PLANE_MAX) memcpy((lsb ? shotLsb : shotMsb) + offset, src, len);
}
void shotGrayShown() { shotShown = true; }
#else
void shotRefresh(bool) {}
void shotPlane(bool, const uint8_t*, uint32_t, uint32_t) {}
void shotGrayShown() {}
#endif
}  // namespace

#define SD_SPI_MISO 7

namespace {
// EInkDisplay::setBusyWaitHooks() only accepts plain function pointers, so these
// forward to the powerManager singleton instead of capturing state.
#if !FREEINK_SD_SDMMC
// The panel needs no SPI traffic while its waveform runs (~0.5 s), so lend the
// shared bus to SD access for that window instead of stalling every read.
bool spiLentDuringBusyWait = false;
#endif

#if CROSSDINK_PERF_LOG
// The begin hook fires once a wait passes the SDK's 20 ms threshold. Waits this
// long are refreshes (PON ~130 ms and POF ~80 ms stay below it).
constexpr uint32_t REFRESH_WAIT_MIN_MS = 250;
uint32_t busyWaitBeganMs = 0;
#endif

void onDisplayBusyWaitBegin() {
#if CROSSDINK_PERF_LOG
  busyWaitBeganMs = millis();
#endif
  powerManager.beginDisplayBusyWait();
#if !FREEINK_SD_SDMMC
  spiLentDuringBusyWait = HalSpiBus::getInstance().releaseForIdle();
#endif
}

void onDisplayBusyWaitEnd() {
#if !FREEINK_SD_SDMMC
  if (spiLentDuringBusyWait) {
    spiLentDuringBusyWait = false;
    HalSpiBus::getInstance().reacquireAfterIdle();
  }
#endif
  powerManager.endDisplayBusyWait();
#if CROSSDINK_PERF_LOG
  if (millis() - busyWaitBeganMs >= REFRESH_WAIT_MIN_MS) PerfLog::noteInk();
#endif
}
#ifndef SIMULATOR
// Trial slice hook: replaces the 1-tick BUSY poll with a 10 ms task sleep, long
// enough for tickless idle to light-sleep (no GPIO wake, so completion is seen
// up to 10 ms late).
bool onDisplayBusyWaitSlice(int8_t, uint8_t) {
  if (!powerManager.refreshLightSleepAllowed()) return false;
  vTaskDelay(pdMS_TO_TICKS(10));
  return true;
}
#endif
}  // namespace

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

HalDisplay::~HalDisplay() {}

void HalDisplay::begin(bool seamless) {
  HalSpiBus::Lock spiLock;

  // Set X3-specific panel mode before initializing.
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }

  einkDisplay.begin();
  // Keep tickless idle from light-sleeping mid-refresh; safe even before
  // powerManager.begin() runs since the hooks no-op until the lock exists.
  einkDisplay.setBusyWaitHooks(&onDisplayBusyWaitBegin, &onDisplayBusyWaitEnd);

  if (seamless) {
    // Defuse the SDK's X3 _x3InitialFullSyncsRemaining counter (no-op on X4)
    // so the first paint isn't promoted to FULL (~770ms). Skips the wakeup-
    // gated requestResync() below for the same reason.
    einkDisplay.skipInitialResync();
    return;
  }
  // Request resync after specific wakeup events to ensure clean display state.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

bool HalDisplay::seedDisplayedFrame(const uint8_t* frame) {
  HalSpiBus::Lock spiLock;
  return einkDisplay.seedDisplayedFrame(frame);
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  einkDisplay.drawImage(imageData, x, y, w, h, fromProgmem);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  einkDisplay.drawImageTransparent(imageData, x, y, w, h, fromProgmem);
}

EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  shotRefresh(false);
  FlashScope flash(*this, mode);
  count(mode);
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::setInverted(bool inverted) {
  HalSpiBus::Lock spiLock;
  einkDisplay.setInverted(inverted);
}

void HalDisplay::displayBufferAsync(HalDisplay::RefreshMode mode) {
  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  shotRefresh(false);
  markFlash(mode);
  count(mode);
  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
}

void HalDisplay::waitRefreshComplete() {
  einkDisplay.waitRefreshComplete();
  flashStart.store(0, std::memory_order_relaxed);
}

void HalDisplay::displayBufferDeferred(HalDisplay::RefreshMode mode) {
  HalSpiBus::Lock spiLock;
  shotRefresh(false);
  markFlash(mode);
  count(mode);
  einkDisplay.displayBufferAsync(convertRefreshMode(mode));
}

bool HalDisplay::isRefreshPending() const { return einkDisplay.isRefreshPending(); }

bool HalDisplay::isRefreshBusy() { return einkDisplay.refreshBusy(); }

bool HalDisplay::supportsAsyncRefresh() const { return einkDisplay.supportsAsyncRefresh(); }

HalDisplay::GrayscaleCapabilities HalDisplay::grayscaleCapabilities(GrayscaleMode mode) const {
  return einkDisplay.grayscaleCapabilities(mode);
}

bool HalDisplay::supportsAsyncGrayscaleBase() const { return grayscaleCapabilities().asyncBase; }

bool HalDisplay::displayGrayscaleBase(GrayscaleMode mode, RefreshMode fallback, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  if (gpio.deviceIsX3() && fallback == HALF_REFRESH) einkDisplay.requestResync(1);
  FlashScope flash(*this, mode == GrayscaleMode::Direct || fallback != FAST_REFRESH,
                   mode == GrayscaleMode::Direct ? FlashKind::Gray : FlashKind::Full);
  shotRefresh(mode != GrayscaleMode::Overlay);
  count(fallback);
  return einkDisplay.displayGrayscaleBase(mode, convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

  shotRefresh(false);
  FlashScope flash(*this, mode);
  count(mode);
  einkDisplay.refreshDisplay(convertRefreshMode(mode), turnOffScreen);
}

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

bool HalDisplay::powerOffIdle() {
  HalSpiBus::Lock spiLock;
  return einkDisplay.powerOffIdle();
}

bool HalDisplay::powerOnIdle() {
#ifndef SIMULATOR  // the simulator panel has no booster
  HalSpiBus::Lock spiLock;
  return einkDisplay.powerOnIdle();
#else
  return false;
#endif
}

void HalDisplay::setRefreshLightSleep(const bool allowed) {
#ifndef SIMULATOR
  powerManager.setRefreshLightSleep(allowed);
  // Installed only for the trial: with a slice hook the SDK polls BUSY instead
  // of taking its edge-interrupt path on other controllers.
  einkDisplay.setBusyWaitSliceHook(allowed ? &onDisplayBusyWaitSlice : nullptr);
#else
  (void)allowed;
#endif
}

void HalDisplay::deepSleep() {
  HalSpiBus::Lock spiLock;
  einkDisplay.deepSleep();
}

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* sizeOut) { return einkDisplay.lendBuildStorage(sizeOut); }

void HalDisplay::returnFrameBufferStorage() { einkDisplay.returnBuildStorage(); }

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  markFlash(!smoothGray, FlashKind::Gray);
  shotPlane(true, lsbBuffer, 0, getBufferSize());
  shotPlane(false, msbBuffer, 0, getBufferSize());
  einkDisplay.copyGrayscaleBuffers(lsbBuffer, msbBuffer);
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  // X3: a HALF or FULL fallback means the caller wants a clean base (e.g. the
  // sleep cover, a full-screen swap from arbitrary prior content). Without
  // this, the X3 grayscale base takes its gentle differential happy path and
  // the prior home/reader frame ghosts through the soft aa_pre_bw_mid
  // waveform. Forcing a resync makes displayGrayscaleBase clear first,
  // matching displayBuffer(HALF)/displayBuffer(FULL).
  if (gpio.deviceIsX3() && fallback != RefreshMode::FAST_REFRESH) {
    einkDisplay.requestResync(1);
  }

  shotRefresh(false);
  FlashScope flash(*this, fallback != RefreshMode::FAST_REFRESH);
  count(fallback);
  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  markFlash(!smoothGray, FlashKind::Gray);
  shotPlane(true, lsbBuffer, 0, getBufferSize());
  einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer);
}

// Refreshes that may flash: Half/Full and full-swing gray passes. On UC8179
// this only keeps the main loop ticking fast; the driver plans
// (flashPlannedMs) and reports (flashStartedMs) the real swing. Other panels
// dim from here.
// Cleared when a refresh finishes; the main loop also drops a stale mark.
void HalDisplay::markFlash(const bool flashes, const FlashKind kind) {
  if (!flashes) return;
  markKind.store(kind, std::memory_order_relaxed);
  flashStart.store(millis() | 1, std::memory_order_relaxed);  // | 1: never 0 while set
  count(FLASHING);
}

void HalDisplay::markFlash(const RefreshMode mode) {
  markFlash(mode != FAST_REFRESH);
}

uint32_t HalDisplay::flashEndsMs() const {
#ifndef SIMULATOR
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179) {
    return freeink::uc8179FlashSwingDoneMs();
  }
#endif
  return 0;
}

HalDisplay::FlashKind HalDisplay::flashKind() const {
#ifndef SIMULATOR
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179) {
    static_assert(static_cast<int>(freeink::Uc8179FlashKind::Paint) == static_cast<int>(FlashKind::Paint) &&
                  static_cast<int>(freeink::Uc8179FlashKind::GrayDark) == static_cast<int>(FlashKind::GrayDark));
    return static_cast<FlashKind>(freeink::uc8179FlashKind());
  }
#endif
  return FlashKind::Full;
}

uint32_t HalDisplay::flashPlannedMs() const {
#ifndef SIMULATOR
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179) {
    return freeink::uc8179FlashPlannedMs();
  }
#endif
  return flashStart.load(std::memory_order_relaxed);
}

HalDisplay::FlashKind HalDisplay::flashPlannedKind() const {
#ifndef SIMULATOR
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179) {
    return static_cast<FlashKind>(freeink::uc8179FlashPlannedKind());
  }
#endif
  return markKind.load(std::memory_order_relaxed);
}

uint32_t HalDisplay::flashStartedMs() const {
#ifndef SIMULATOR
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179) {
    return freeink::uc8179FlashSwingMs();
  }
#endif
  return flashStart.load(std::memory_order_relaxed);
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  shotPlane(false, msbBuffer, 0, getBufferSize());
  einkDisplay.copyGrayscaleMsbBuffers(msbBuffer);
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  flashStart.store(0, std::memory_order_relaxed);
  einkDisplay.cleanupGrayscaleBuffers(bwBuffer);
}

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  FlashScope flash(*this, false);  // clears the mark its planes set
  count(GRAY_PASSES);
  einkDisplay.displayGrayBuffer(turnOffScreen);
  shotGrayShown();
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  HalSpiBus::Lock spiLock;
  shotPlane(lsbPlane, rows, static_cast<uint32_t>(yStart) * getDisplayWidthBytes(),
             static_cast<uint32_t>(numRows) * getDisplayWidthBytes());
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

void HalDisplay::setInvertedTextGray(const bool enabled) {
#ifndef SIMULATOR
  einkDisplay.setInvertedTextGray(enabled);
#else
  (void)enabled;
#endif
}

void HalDisplay::setSmoothGray(const bool smooth) {
  smoothGray = smooth;
#ifndef SIMULATOR  // the simulator panel has no waveform choice
  einkDisplay.setSmoothGray(smooth);
#else
  (void)smooth;
#endif
}

bool HalDisplay::shouldSkipImageBlanking() const {
  // CrossDink's extra white-image pass is redundant on UC8179. Its driver
  // always supports async display; the existing query also excludes inverted
  // output, a pending inversion transition, and an uninitialized driver.
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179 &&
         einkDisplay.supportsAsyncRefresh();
}

bool HalDisplay::fastTracksPanel() const {
  return BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179;
}

bool HalDisplay::displayGrayscaleBaseAsync(HalDisplay::RefreshMode fallback) {
  HalSpiBus::Lock spiLock;
  shotRefresh(false);
  markFlash(fallback != RefreshMode::FAST_REFRESH);
  count(fallback);
  return einkDisplay.displayGrayscaleBaseAsync(convertRefreshMode(fallback));
}

bool HalDisplay::supportsDeferredGrayscaleBase() const { return einkDisplay.supportsDeferredGrayscaleBase(); }

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }

bool HalDisplay::grayShotReady() const {
#if GRAY_SHOT
  return shotShown && getBufferSize() <= SHOT_PLANE_MAX;
#else
  return false;
#endif
}

uint8_t HalDisplay::grayShotLevel(const uint32_t x, const uint32_t y) const {
#if GRAY_SHOT
  // Levels per GrayscaleCapabilities.h. Overlay masks (LSB, MSB): dark=11,
  // light=01, else the B/W framebuffer (1 = white), which holds the page base
  // again once a gray pass returns. Absolute planes: level = LSB + 2 * MSB.
  const uint32_t i = y * getDisplayWidthBytes() + (x >> 3);
  const uint8_t bit = 0x80 >> (x & 7);
  const bool lsb = shotLsb[i] & bit;
  const bool msb = shotMsb[i] & bit;
  if (shotAbsolute) return lsb + 2 * msb;
  return msb ? (lsb ? 1 : 2) : ((getFrameBuffer()[i] & bit) ? 3 : 0);
#else
  (void)x;
  (void)y;
  return 0;
#endif
}
