#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <PerfLog.h>

#include "HalSpiBus.h"

// Global HalDisplay instance
HalDisplay display;

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

  einkDisplay.displayBufferAsyncNoShadow(convertRefreshMode(mode));
}

void HalDisplay::waitRefreshComplete() { einkDisplay.waitRefreshComplete(); }

void HalDisplay::displayBufferDeferred(HalDisplay::RefreshMode mode) {
  HalSpiBus::Lock spiLock;
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
  return einkDisplay.displayGrayscaleBase(mode, convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::refreshDisplay(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  HalSpiBus::Lock spiLock;

  if (gpio.deviceIsX3() && mode == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }

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

  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  einkDisplay.preconditionGrayscale(x, y, w, h);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  HalSpiBus::Lock spiLock;
  einkDisplay.displayGrayBuffer(turnOffScreen);
}

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  HalSpiBus::Lock spiLock;
  einkDisplay.writeGrayscalePlaneStrip(lsbPlane ? EInkDisplay::GRAY_PLANE_LSB : EInkDisplay::GRAY_PLANE_MSB, rows,
                                       yStart, numRows);
}

void HalDisplay::setSmoothGray(const bool smooth) {
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

bool HalDisplay::displayGrayscaleBaseAsync(HalDisplay::RefreshMode fallback) {
  HalSpiBus::Lock spiLock;
  return einkDisplay.displayGrayscaleBaseAsync(convertRefreshMode(fallback));
}

bool HalDisplay::supportsDeferredGrayscaleBase() const { return einkDisplay.supportsDeferredGrayscaleBase(); }

bool HalDisplay::supportsStripGrayscale() const { return einkDisplay.supportsStripGrayscale(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }

uint32_t HalDisplay::getBufferSize() const { return einkDisplay.getBufferSize(); }
