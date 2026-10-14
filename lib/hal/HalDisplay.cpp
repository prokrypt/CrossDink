#include <BoardConfig.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <Logging.h>
#include <PerfLog.h>
#include <PowerCounters.h>

#include "HalSpiBus.h"

#ifndef SIMULATOR
#include <driver/gpio.h>
#include <esp_sleep.h>
#include <freertos/semphr.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_struct.h>
#endif

// Global HalDisplay instance
HalDisplay display;

#if CROSSDINK_GOODIES
namespace {
RTC_NOINIT_ATTR HalDisplay::RefreshCounts rtcRefreshCounts;  // survives deep sleep; random after power loss
constexpr uint32_t REFRESH_COUNTS_MAGIC = 0x52465243;        // "RFRC"
}  // namespace

HalDisplay::RefreshCounts& HalDisplay::refreshCounts() {
  if (rtcRefreshCounts.magic != REFRESH_COUNTS_MAGIC) rtcRefreshCounts = {REFRESH_COUNTS_MAGIC, {}};
  return rtcRefreshCounts;
}

void HalDisplay::count(const int kind) {
  refreshCounts().n[kind]++;
  if (kind == FLASHING) return;  // a mark on a refresh counted on its own
  // RefreshMode and GRAY_PASSES share PowerCounters::PanelKind's numbering.
  PowerCounters::panelKind(static_cast<PowerCounters::PanelKind>(kind));
  // UC8179 keeps its booster up after a refresh until powerOffIdle(); the other
  // controllers power down inside each refresh, so only it gets booster time.
  if (BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179) PowerCounters::booster(true);
}
static_assert(static_cast<int>(HalDisplay::FULL_REFRESH) == PowerCounters::PANEL_FULL &&
                  static_cast<int>(HalDisplay::HALF_REFRESH) == PowerCounters::PANEL_HALF &&
                  static_cast<int>(HalDisplay::FAST_REFRESH) == PowerCounters::PANEL_FAST &&
                  static_cast<int>(HalDisplay::GRAY_PASSES) == PowerCounters::PANEL_GRAY,
              "PowerCounters::PanelKind follows HalDisplay's refresh kinds");
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
// long are refreshes (PON ~130 ms and POF ~80 ms stay below it). A wait that
// begins with a deferred refresh pending is that refresh ending, however short:
// PON runs before the refresh is marked pending, POF only once none is.
constexpr uint32_t REFRESH_WAIT_MIN_MS = 250;
uint32_t busyWaitBeganMs = 0;
bool busyWaitEndsRefresh = false;
#endif

void onDisplayBusyWaitBegin() {
#if CROSSDINK_PERF_LOG
  busyWaitBeganMs = millis();
  busyWaitEndsRefresh = display.isRefreshPending();
#endif
  PowerCounters::panelBusy(true);
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
  PowerCounters::panelBusy(false);
#if CROSSDINK_PERF_LOG
  // Still pending here: this wait finished an async refresh an earlier call started.
  if (busyWaitEndsRefresh || millis() - busyWaitBeganMs >= REFRESH_WAIT_MIN_MS) {
    PerfLog::noteInk(display.isRefreshPending());
  }
#endif
}
#ifndef SIMULATOR
// Slice hook: instead of the 1-tick BUSY poll, arm BUSY's idle level as a level
// interrupt (the only kind that also ends a light sleep) and block until it
// fires, so tickless idle can light-sleep through the waveform and the wait
// still ends on the pin itself. The cap only bounds a lost interrupt.
constexpr uint32_t BUSY_SLICE_MAX_MS = 100;
StaticSemaphore_t busyIdleSignalBuf;
SemaphoreHandle_t busyIdleSignal = nullptr;
int8_t busyIsrPin = -1;  // pin the handler is attached to

// A level keeps firing while it holds, so the line disarms itself here.
void IRAM_ATTR onBusyIdle(void* arg) {
  gpio_ll_intr_disable(&GPIO, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg)));
  BaseType_t higherPriorityTaskWoken = pdFALSE;
  xSemaphoreGiveFromISR(busyIdleSignal, &higherPriorityTaskWoken);
  if (higherPriorityTaskWoken == pdTRUE) portYIELD_FROM_ISR();
}

bool onDisplayBusyWaitSlice(const int8_t busyPin, const uint8_t busyLevel) {
  if (!powerManager.refreshLightSleepAllowed() || busyPin < 0) return false;
  const auto pin = static_cast<gpio_num_t>(busyPin);
  if (busyIsrPin != busyPin) {
    if (busyIdleSignal == nullptr) busyIdleSignal = xSemaphoreCreateBinaryStatic(&busyIdleSignalBuf);
    gpio_intr_disable(pin);
    // InputWake::begin() installed the GPIO ISR service.
    if (gpio_isr_handler_add(pin, onBusyIdle, reinterpret_cast<void*>(static_cast<uintptr_t>(busyPin))) != ESP_OK) {
      LOG_ERR("EPD", "No BUSY interrupt on GPIO%d; refreshes keep polling without light sleep", busyPin);
      powerManager.setRefreshLightSleep(false);
      return false;
    }
    busyIsrPin = busyPin;
    esp_sleep_enable_gpio_wakeup();  // InputWake turns it on too, when the board has wake lines
  }
  xSemaphoreTake(busyIdleSignal, 0);  // a fire left over from the last slice
  gpio_wakeup_enable(pin, busyLevel == LOW ? GPIO_INTR_HIGH_LEVEL : GPIO_INTR_LOW_LEVEL);
  gpio_intr_enable(pin);
  if (xSemaphoreTake(busyIdleSignal, pdMS_TO_TICKS(BUSY_SLICE_MAX_MS)) != pdTRUE && gpio_get_level(pin) != busyLevel) {
    LOG_ERR("EPD", "BUSY wake missed: idle seen at the %lu ms cap", static_cast<unsigned long>(BUSY_SLICE_MAX_MS));
  }
  gpio_intr_disable(pin);
  // Armed at the idle level BUSY now holds, an RTC IO wake would reject every light sleep.
  gpio_wakeup_disable(pin);
  // gpio_wakeup_enable() left a level type, which a later pinMode() (or one after a restart) re-enables.
  gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
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
#if CONFIG_PM_ENABLE
  // Except UC8179 waits: they poll BUSY, so the slice hook light-sleeps them
  // and wakes on the pin. Other panels keep the SDK's edge-interrupt wait,
  // whose edge a light sleep would miss.
  setRefreshLightSleep(BoardConfig::ACTIVE.displayController == BoardConfig::DisplayController::UC8179);
#endif

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
  if (turnOffScreen) PowerCounters::booster(false);
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
  const bool shown = einkDisplay.displayGrayscaleBase(mode, convertRefreshMode(fallback), turnOffScreen);
  if (turnOffScreen) PowerCounters::booster(false);
  return shown;
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
  if (turnOffScreen) PowerCounters::booster(false);
}

bool HalDisplay::isInverted() const { return einkDisplay.isInverted(); }

bool HalDisplay::powerOffIdle() {
  HalSpiBus::Lock spiLock;
  const bool off = einkDisplay.powerOffIdle();
  if (off) PowerCounters::booster(false);
  return off;
}

bool HalDisplay::powerOnIdle() {
#ifndef SIMULATOR  // the simulator panel has no booster
  HalSpiBus::Lock spiLock;
  const bool on = einkDisplay.powerOnIdle();
  if (on) PowerCounters::booster(true);
  return on;
#else
  return false;
#endif
}

void HalDisplay::setRefreshLightSleep(const bool allowed) {
#ifndef SIMULATOR
  powerManager.setRefreshLightSleep(allowed);
  // Installed only when allowed: with a slice hook the SDK polls BUSY instead
  // of taking its edge-interrupt path on other controllers.
  einkDisplay.setBusyWaitSliceHook(allowed ? &onDisplayBusyWaitSlice : nullptr);
#else
  (void)allowed;
#endif
}

void HalDisplay::deepSleep() {
  HalSpiBus::Lock spiLock;
  einkDisplay.deepSleep();
  PowerCounters::booster(false);
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
  if (turnOffScreen) PowerCounters::booster(false);
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

void HalDisplay::markFlash(const RefreshMode mode) { markFlash(mode != FAST_REFRESH); }

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
  if (turnOffScreen) PowerCounters::booster(false);
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
