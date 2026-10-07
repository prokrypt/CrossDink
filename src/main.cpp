#include <Arduino.h>
#include <BoardConfig.h>
#include <CrossDinkHalFrontlight.h>
#include <Epub/blocks/ImageBlock.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Knobs.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PerfLog.h>
#include <SPI.h>
#include <WiFi.h>
#if !defined(SIMULATOR) && !FREEINK_MCU_C3
#include <XteinkDetect.h>
#endif
#ifndef SIMULATOR
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_timer.h>
#endif
#include <builtinFonts/all.h>
#include <uzlib.h>
#if !defined(SIMULATOR)
#include <esp_cache.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

#include "AppCapabilities.h"
#include "util/BootReason.h"
#include "util/BuildInfo.h"

#ifndef SIMULATOR
#include <nvs.h>
#endif

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "GlobalActions.h"
#include "KOReaderCredentialStore.h"
#include "Knobs.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "SilentRestart.h"
#if CROSSDINK_GOODIES
#include "activities/goodies/GoodiesActivity.h"
#endif
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/RenderLock.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "activities/home/BookActions.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/reader/KOSyncAuto.h"
#include "activities/reader/ReaderExitSave.h"
#include "activities/reader/ReaderProgressShadow.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "activities/reader/StatsBackup.h"
#include "activities/settings/FontDownloadActivity.h"
#include "activities/settings/KOReaderAuthActivity.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#include "activities/settings/OtaUpdateActivity.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "components/icons/tablerFilledIcons.h"
#include "components/themes/BaseTheme.h"
#include "fontIds.h"
#include "network/SerialRemote.h"
#include "network/UsbSerialFileTransfer.h"
#include "network/WifiBackgroundJoin.h"
#include "network/WifiUtils.h"
#include "platform/InputTask.h"
#include "platform/InputWake.h"
#include "platform/PinMon.h"
#ifdef SIMULATOR
#include <SimulatorLifecycle.h>

#include "simulator/SimulatorHomeKeyInput.h"
#include "simulator/SimulatorSmokeTest.h"
#endif
#include "util/BatteryDiagnosticLog.h"
#include "util/BatteryLog.h"
#include "util/ButtonNavigator.h"
#include "util/ButtonShortcutController.h"
#include "util/CoreLoadLog.h"
#include "util/DeviceIdentity.h"
#include "util/DeviceSecurity.h"
#include "util/Dictionary.h"
#include "util/DictionaryRegistry.h"
#include "util/FrontlightSchedule.h"
#include "util/LocalClock.h"
#include "util/ScreenshotUtil.h"
#include "util/SleepLog.h"
#include "util/SleepWakePolicy.h"
#include "util/TouchNoiseMonitor.h"
#include "util/TransferLightPulse.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
DictionaryRegistry dictionaryRegistry;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
constexpr unsigned long BOOT_SLEEP_GRACE_MS = 2000;  // power actions ignored this long after boot
static ButtonShortcutController buttonShortcutController;
static unsigned long lastX4ProHomeKeyTapAt = 0;
static bool x4ProHomeKeyTapPending = false;
// A held power button can span deep-sleep wake and the first main-loop frame.
// Do not treat that wake gesture as an in-session shortcut until it has been released.
static bool powerButtonReleasedSinceWake = false;
// Wake can continue once its hold has been verified. Swallow the release that
// ends that wake gesture so it cannot become an in-session button action.
static bool wakePowerReleasePending = false;

namespace {
KNOB_ALIAS(X4PRO_HOME_KEY_DOUBLE_TAP_MS, homeDoubleTapMs);  // Goodies > Knobs

struct QuickLockBadgeBackdrop {
  static constexpr int SIZE = 40;
  // A 40 px square can straddle at most six 1-bit framebuffer bytes per row.
  static constexpr size_t MAX_BYTES = 6 * SIZE;

  std::array<uint8_t, MAX_BYTES> pixels{};
  int x = 0;
  int y = 0;
  size_t size = 0;
  GfxRenderer::Orientation orientation = GfxRenderer::Portrait;
  bool valid = false;

  void save(GfxRenderer& renderer, const int savedX, const int savedY) {
    x = savedX;
    y = savedY;
    orientation = renderer.getOrientation();
    size = renderer.getRegionByteSize(x, y, SIZE, SIZE);
    valid = size > 0 && size <= pixels.size() && renderer.copyRegionToBuffer(x, y, SIZE, SIZE, pixels.data(), size);
  }

  bool restore(GfxRenderer& renderer) {
    if (!valid || renderer.getOrientation() != orientation) return false;
    valid = false;
    return renderer.copyBufferToRegion(x, y, SIZE, SIZE, pixels.data(), size);
  }
};

QuickLockBadgeBackdrop quickLockBadgeBackdrop;
}  // namespace

#if !defined(SIMULATOR) && CONFIG_SPIRAM && !CONFIG_SPIRAM_BOOT_INIT
// Arduino's psramInit() runs esp_psram_extram_test() on every boot, including
// every deep-sleep wake: a write-then-read sweep of the whole 8 MB part before
// setup() starts. esp_psram_init() has already identified the chip by then, so
// skip the sweep and keep the wake path short.
extern "C" bool testSPIRAM(void) { return true; }
#endif

#if defined(CROSSDINK_LOOP_STACK_BYTES) && !defined(SIMULATOR)
// Overrides FreeInkUI's weak 16 KB loopTask stack (internal RAM).
size_t getArduinoLoopTaskStackSize(void) { return CROSSDINK_LOOP_STACK_BYTES; }
#endif

static void logBootHeap(const char* stage) {
  LOG_DBG("BOOTMEM", "%s: free=%" PRIu32 " maxAlloc=%" PRIu32, stage, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Fonts
#if !CROSSDINK_SCALABLE_FONTS
EpdFont lexenddeca10RegularFont(&lexenddeca_10_regular);
EpdFont lexenddeca10BoldFont(&lexenddeca_10_bold);
EpdFont lexenddeca10ItalicFont(&lexenddeca_10_italic);
EpdFont lexenddeca10BoldItalicFont(&lexenddeca_10_bolditalic);
EpdFontFamily lexenddeca10FontFamily(&lexenddeca10RegularFont, &lexenddeca10BoldFont, &lexenddeca10ItalicFont,
                                     &lexenddeca10BoldItalicFont);
EpdFont lexenddeca12RegularFont(&lexenddeca_12_regular);
EpdFont lexenddeca12BoldFont(&lexenddeca_12_bold);
EpdFont lexenddeca12ItalicFont(&lexenddeca_12_italic);
EpdFont lexenddeca12BoldItalicFont(&lexenddeca_12_bolditalic);
EpdFontFamily lexenddeca12FontFamily(&lexenddeca12RegularFont, &lexenddeca12BoldFont, &lexenddeca12ItalicFont,
                                     &lexenddeca12BoldItalicFont);
EpdFont lexenddeca14RegularFont(&lexenddeca_14_regular);
EpdFont lexenddeca14BoldFont(&lexenddeca_14_bold);
EpdFont lexenddeca14ItalicFont(&lexenddeca_14_italic);
EpdFont lexenddeca14BoldItalicFont(&lexenddeca_14_bolditalic);
EpdFontFamily lexenddeca14FontFamily(&lexenddeca14RegularFont, &lexenddeca14BoldFont, &lexenddeca14ItalicFont,
                                     &lexenddeca14BoldItalicFont);
EpdFont lexenddeca16RegularFont(&lexenddeca_16_regular);
EpdFont lexenddeca16BoldFont(&lexenddeca_16_bold);
EpdFont lexenddeca16ItalicFont(&lexenddeca_16_italic);
EpdFont lexenddeca16BoldItalicFont(&lexenddeca_16_bolditalic);
EpdFontFamily lexenddeca16FontFamily(&lexenddeca16RegularFont, &lexenddeca16BoldFont, &lexenddeca16ItalicFont,
                                     &lexenddeca16BoldItalicFont);
EpdFont bitter10RegularFont(&bitter_10_regular);
EpdFont bitter10BoldFont(&bitter_10_bold);
EpdFont bitter10ItalicFont(&bitter_10_italic);
EpdFont bitter10BoldItalicFont(&bitter_10_bolditalic);
EpdFontFamily bitter10FontFamily(&bitter10RegularFont, &bitter10BoldFont, &bitter10ItalicFont, &bitter10BoldItalicFont);
EpdFont bitter12RegularFont(&bitter_12_regular);
EpdFont bitter12BoldFont(&bitter_12_bold);
EpdFont bitter12ItalicFont(&bitter_12_italic);
EpdFont bitter12BoldItalicFont(&bitter_12_bolditalic);
EpdFontFamily bitter12FontFamily(&bitter12RegularFont, &bitter12BoldFont, &bitter12ItalicFont, &bitter12BoldItalicFont);
EpdFont bitter14RegularFont(&bitter_14_regular);
EpdFont bitter14BoldFont(&bitter_14_bold);
EpdFont bitter14ItalicFont(&bitter_14_italic);
EpdFont bitter14BoldItalicFont(&bitter_14_bolditalic);
EpdFontFamily bitter14FontFamily(&bitter14RegularFont, &bitter14BoldFont, &bitter14ItalicFont, &bitter14BoldItalicFont);
EpdFont bitter16RegularFont(&bitter_16_regular);
EpdFont bitter16BoldFont(&bitter_16_bold);
EpdFont bitter16ItalicFont(&bitter_16_italic);
EpdFont bitter16BoldItalicFont(&bitter_16_bolditalic);
EpdFontFamily bitter16FontFamily(&bitter16RegularFont, &bitter16BoldFont, &bitter16ItalicFont, &bitter16BoldItalicFont);

#endif
EpdFont smallFont(&inter_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

const EpdFont uiSymbols10Font(&ui_symbols_10);

EpdFont ui10RegularFont(&inter_10_regular);
EpdFont ui10BoldFont(&inter_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont, nullptr, nullptr, &uiSymbols10Font);

EpdFont ui12RegularFont(&inter_12_regular);
EpdFont ui12BoldFont(&inter_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont, nullptr, nullptr, &uiSymbols10Font);

const char* wakeupRouteName(const HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "PowerButton";
    case HalGPIO::WakeupReason::AfterFlash:
      return "AfterFlash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "AfterUSBPower";
    case HalGPIO::WakeupReason::Other:
    default:
      return "Other";
  }
}

void logMemoryStats(const char* phase) {
#if defined(BOARD_HAS_PSRAM)
  LOG_INF("MEM", "%s: heap free=%u total=%u min=%u maxAlloc=%u psram free=%u total=%u min=%u maxAlloc=%u", phase,
          ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(), ESP.getFreePsram(),
          ESP.getPsramSize(), ESP.getMinFreePsram(), ESP.getMaxAllocPsram());
#else
  LOG_INF("MEM", "%s: heap free=%u total=%u min=%u maxAlloc=%u", phase, ESP.getFreeHeap(), ESP.getHeapSize(),
          ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
#endif
}

// Memory part of the 2 s [SYS] line: empty unless heap or PSRAM moved or the
// heap low-water mark dropped. The constant totals are in the [MEM] Boot line;
// PSRAM min/maxAlloc rarely move and print only when they did.
void formatPeriodicMemory(char* out, const size_t size) {
  constexpr uint32_t PERIODIC_HEAP_DELTA = 1024;
  constexpr uint32_t PERIODIC_PSRAM_DELTA = 8 * 1024;
  static bool hasPrevious = false;
  static uint32_t previousFreeHeap = 0;
  static uint32_t previousMinFreeHeap = 0;
  const auto movedBy = [](const uint32_t a, const uint32_t b, const uint32_t delta) {
    return (a > b ? a - b : b - a) >= delta;
  };

  out[0] = '\0';
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t minFreeHeap = ESP.getMinFreeHeap();
  bool moved =
      !hasPrevious || movedBy(freeHeap, previousFreeHeap, PERIODIC_HEAP_DELTA) || minFreeHeap != previousMinFreeHeap;
#if defined(BOARD_HAS_PSRAM)
  static uint32_t previousFreePsram = 0;
  const uint32_t freePsram = ESP.getFreePsram();
  moved = moved || movedBy(freePsram, previousFreePsram, PERIODIC_PSRAM_DELTA);
#endif
  if (!moved) return;

  hasPrevious = true;
  previousFreeHeap = freeHeap;
  previousMinFreeHeap = minFreeHeap;
#if defined(BOARD_HAS_PSRAM)
  static uint32_t previousMinPsram = 0;
  static uint32_t previousMaxAllocPsram = 0;
  previousFreePsram = freePsram;
  const uint32_t minPsram = ESP.getMinFreePsram();
  const uint32_t maxAllocPsram = ESP.getMaxAllocPsram();
  const int written = snprintf(out, size, " heap free=%u min=%u maxAlloc=%u psram free=%u", freeHeap, minFreeHeap,
                               ESP.getMaxAllocHeap(), freePsram);
  if (written > 0 && static_cast<size_t>(written) < size &&
      (minPsram != previousMinPsram || maxAllocPsram != previousMaxAllocPsram)) {
    previousMinPsram = minPsram;
    previousMaxAllocPsram = maxAllocPsram;
    snprintf(out + written, size - written, " min=%u maxAlloc=%u", minPsram, maxAllocPsram);
  }
#else
  snprintf(out, size, " heap free=%u min=%u maxAlloc=%u", freeHeap, minFreeHeap, ESP.getMaxAllocHeap());
#endif
}

// One [SYS] line every 2 s with whichever parts have news: memory, then SD and
// image counters ([PERF] before), then core load with its task list last
// ([CPU] before). Each part keeps its old field names. The parts append into
// one 384 B stack buffer on loopTask (its [STK] headroom is logged).
void logSystemLine() {
  char line[384];
  formatPeriodicMemory(line, sizeof(line));
  size_t used = strlen(line);
  PerfLog::logPeriodic(line + used, sizeof(line) - used);
  used += strlen(line + used);
  CoreLoadLog::formatSinceLast(line + used, sizeof(line) - used);
  if (line[0] != '\0') LOG_INF("SYS", "%s", line + 1);
}

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
RTC_NOINIT_ATTR uint32_t silentRebootPayload;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildMagic;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildBookHash;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildPackedTarget;
RTC_NOINIT_ATTR uint32_t silentReaderPageBuildFlags;
RTC_NOINIT_ATTR uint32_t silentFirmwareUpdateMagic;
RTC_NOINIT_ATTR char silentFirmwareUpdatePath[MAX_SILENT_FIRMWARE_PATH];
RTC_NOINIT_ATTR uint32_t silentRebootFrontlight;
// Armed by enterDeepSleep() and consumed at the next boot, so the power-button
// wake right after a sleep can skip the splash. Keeping it in RTC rather than
// state.json saves an SD write on every wake; power loss clears it, so a cold
// boot shows the splash.
RTC_NOINIT_ATTR uint32_t splashlessWakeMagic;
constexpr uint32_t SPLASHLESS_WAKE_MAGIC = 0x534C5750;         // "SLWP"
constexpr uint32_t SILENT_FIRMWARE_UPDATE_MAGIC = 0x46574E55;  // "FWNU"
constexpr uint32_t SILENT_REBOOT_FRONTLIGHT_OFF = 0xC1EA1100;
constexpr uint32_t SILENT_REBOOT_FRONTLIGHT_ON = 0xC1EA1101;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
constexpr uint32_t SILENT_REBOOT_READER_CLEAN_IMAGE_BASE = 1U << 0;
constexpr uint32_t SILENT_READER_PAGE_BUILD_MAGIC = 0xC1EAB017;
constexpr uint32_t SILENT_READER_PAGE_BUILD_AUTO_TURN = 1U << 0;
constexpr uint32_t NETWORK_RENDER_TASK_STACK_BYTES = 8192;
// FreeType's anti-aliased rasterizer reserves a 16 KiB scratch pool on its
// caller's stack. Scalable fonts are S3-only, so leave C3's constrained reader
// stack unchanged and give S3 reader renders room for their normal call frames.
constexpr uint32_t READER_RENDER_TASK_STACK_BYTES = FREEINK_MCU_S3 ? 24576 : 16384;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and leave the panel holding its pre-boot frame; a
// plain boot shows the splash. See setup() for the resolution.
using BootResume = SleepWakePolicy::Resume;

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY && !defined(SIMULATOR)
// The frame on the panel at a silent restart, so the first paint after it can
// be a Fast refresh instead of a full flash (the controller forgets its OLD
// plane on reset). PSRAM noinit survives ESP.restart() and is skipped by the
// boot memory test; the CRC rejects power-on garbage.
struct RetainedPanelFrame {
  static constexpr uint32_t MAGIC = 0x46524D31;  // "FRM1"
  static constexpr size_t CAPACITY = 64 * 1024;
  uint32_t magic;
  uint32_t size;
  uint32_t crc;
  uint8_t bytes[CAPACITY];
};
EXT_RAM_NOINIT_ATTR RetainedPanelFrame retainedPanelFrame;

static void retainPanelFrame() {
  retainedPanelFrame.magic = 0;
  const uint8_t* frame = display.getFrameBuffer();
  const size_t size = display.getBufferSize();
  // Inverted frames are flipped in place only while they are sent.
  // Direct gray on the panel: the B/W framebuffer is not its state, and a Fast
  // first paint on that OLD plane would drive the gray pixels one way.
  if (!frame || size == 0 || size > RetainedPanelFrame::CAPACITY || SETTINGS.screenInverted != 0 ||
      display.grayOnPanel()) {
    esp_cache_msync(&retainedPanelFrame, sizeof(retainedPanelFrame.magic),
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    return;
  }
  memcpy(retainedPanelFrame.bytes, frame, size);
  retainedPanelFrame.size = static_cast<uint32_t>(size);
  retainedPanelFrame.crc = uzlib_crc32(retainedPanelFrame.bytes, static_cast<unsigned int>(size), 0);
  retainedPanelFrame.magic = RetainedPanelFrame::MAGIC;
  // ESP.restart() does not write back the PSRAM cache, so without this the
  // frame (or its header) could still sit in dirty cache lines and be lost;
  // logs showed every restart falling back to a full first paint.
  esp_cache_msync(&retainedPanelFrame, offsetof(RetainedPanelFrame, bytes) + size,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

static bool retainedPanelFramePresent() { return retainedPanelFrame.magic == RetainedPanelFrame::MAGIC; }
// A boot that does not seed the frame (a crash reset before display setup)
// must drop it: a later plain ESP.restart() would otherwise seed a stale frame
// as the OLD plane, and the first Fast paint would re-drive pixels one way.
static void discardRetainedPanelFrame() { retainedPanelFrame.magic = 0; }

// True once this boot loaded the retained frame as the panel's OLD plane.
static bool retainedPanelFrameSeeded = false;

static void seedRetainedPanelFrame() {
  const bool present = retainedPanelFrame.magic == RetainedPanelFrame::MAGIC;
  retainedPanelFrame.magic = 0;
  const size_t size = display.getBufferSize();
  if (!present) {
    LOG_INF("MAIN", "No retained panel frame; first paint full");
    return;
  }
  if (retainedPanelFrame.size != size ||
      uzlib_crc32(retainedPanelFrame.bytes, static_cast<unsigned int>(size), 0) != retainedPanelFrame.crc) {
    LOG_INF("MAIN", "Retained panel frame rejected (size %lu, expected %u, or CRC); first paint full",
            static_cast<unsigned long>(retainedPanelFrame.size), static_cast<unsigned>(size));
    return;
  }
  const bool seeded = display.seedDisplayedFrame(retainedPanelFrame.bytes);
  retainedPanelFrameSeeded = seeded;
  LOG_INF("MAIN", "Retained panel frame %s; first paint %s", seeded ? "loaded" : "unused", seeded ? "fast" : "full");
}
#else
static void retainPanelFrame() {}
static bool retainedPanelFramePresent() { return false; }
static constexpr bool retainedPanelFrameSeeded = false;
static void discardRetainedPanelFrame() {}
static void seedRetainedPanelFrame() {}
#endif

#ifndef SIMULATOR
// Every esp_restart() (silent restart, OTA, SD update, remote REBOOT) runs this:
// it waits out any refresh, then powers the panel off (POF + deep sleep) so the
// booster is not left on until the reset. The retained frame was already
// copied by the caller; begin() resets the controller after the reboot.
// The render lock keeps the render task from refreshing meanwhile; a caller
// that already holds it (render task) is the only one that could refresh.
static void powerOffPanelOnRestart() {
  // ponytail: 3 s covers the longest refresh (sleep cover ~2.7 s) and stays
  // under the 5 s task watchdog; a longer hold leaves the panel on.
  constexpr unsigned long RENDER_LOCK_WAIT_MS = 3000;
  const bool ownLock = RenderLock::heldByCaller();
  RenderLock lock(ownLock ? 0UL : RENDER_LOCK_WAIT_MS);
  if (!ownLock && !lock.ownsLock()) {
    LOG_ERR("MAIN", "Render busy at restart; panel left powered");
    return;
  }
  display.deepSleep();
  LOG_INF("MAIN", "Panel powered off before restart");
}
#endif

void restartKeepingPanelFrame() {
  PerfLog::noteRestart();
  retainPanelFrame();
  ESP.restart();
}

static void restartWithSilentToken() {
  PerfLog::noteRestart();
  retainPanelFrame();
  // SETTINGS.frontlightOn only tracks explicit toggles; wake and schedule
  // policy change the light without saving it, so hand the live state over.
  // A transfer pulse turns the light on for itself: hand over the user's state.
  TransferLightPulse::yieldToUser();
  silentRebootFrontlight = Frontlight.isOn() ? SILENT_REBOOT_FRONTLIGHT_ON : SILENT_REBOOT_FRONTLIGHT_OFF;
#ifdef SIMULATOR
  SimulatorLifecycle::setSilentRebootToken(silentRebootMagic, silentRebootTarget, silentRebootPayload);
#endif
  ESP.restart();
}

static uint32_t silentRestartBookHash(const std::string& bookPath) {
  return uzlib_crc32(bookPath.data(), static_cast<unsigned int>(bookPath.size()), 0);
}

static void clearSilentRestartReaderPageBuild() {
  silentReaderPageBuildMagic = 0;
  silentReaderPageBuildBookHash = 0;
  silentReaderPageBuildPackedTarget = 0;
  silentReaderPageBuildFlags = 0;
}

static void silentRestartToHome(const uint32_t payload, const char* const description) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  clearSilentRestartReaderPageBuild();
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootPayload = payload;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (%s)", description);
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  restartWithSilentToken();
}

void silentRestart() { silentRestartToHome(0, "target=home"); }

void restartToHomeAfterStorageHandoff() {
  // Keep this distinct from other callers: USB Drive has released raw SD
  // storage and must reboot into Home before any normal filesystem work runs.
  silentRestart();
}

void armSilentRestartReaderPageBuild(const std::string& bookPath, const uint16_t spineIndex, const uint16_t targetPage,
                                     const bool autoPageTurnActive) {
  silentReaderPageBuildBookHash = silentRestartBookHash(bookPath);
  silentReaderPageBuildPackedTarget = (static_cast<uint32_t>(spineIndex) << 16) | targetPage;
  silentReaderPageBuildFlags = autoPageTurnActive ? SILENT_READER_PAGE_BUILD_AUTO_TURN : 0;
  silentReaderPageBuildMagic = SILENT_READER_PAGE_BUILD_MAGIC;
}

bool consumeSilentRestartReaderPageBuild(const std::string& bookPath, uint16_t& spineIndex, uint16_t& targetPage,
                                         bool& autoPageTurnActive) {
  const bool matches = silentReaderPageBuildMagic == SILENT_READER_PAGE_BUILD_MAGIC &&
                       silentReaderPageBuildBookHash == silentRestartBookHash(bookPath);
  const uint32_t packedTarget = silentReaderPageBuildPackedTarget;
  const uint32_t flags = silentReaderPageBuildFlags;
  clearSilentRestartReaderPageBuild();
  if (!matches) {
    autoPageTurnActive = false;
    return false;
  }

  spineIndex = static_cast<uint16_t>(packedTarget >> 16);
  targetPage = static_cast<uint16_t>(packedTarget & 0xFFFFU);
  autoPageTurnActive = (flags & SILENT_READER_PAGE_BUILD_AUTO_TURN) != 0;
  return true;
}

static void silentRestartToReaderImpl(const bool cleanImageBaseOnEntry) {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootPayload = cleanImageBaseOnEntry ? SILENT_REBOOT_READER_CLEAN_IMAGE_BASE : 0;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader cleanImageBase=%d)", cleanImageBaseOnEntry ? 1 : 0);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  restartWithSilentToken();
}

void silentRestartToReader(const bool cleanImageBaseOnEntry) { silentRestartToReaderImpl(cleanImageBaseOnEntry); }

static void restartToNetworkTarget(const NetworkBootTarget target, const uint32_t payload) {
  if (deepSleepInProgress) return;
  clearSilentRestartReaderPageBuild();
  silentRebootTarget = static_cast<uint32_t>(target);
  silentRebootPayload = payload;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=network/%lu payload=%lu)", static_cast<unsigned long>(silentRebootTarget),
          static_cast<unsigned long>(payload));
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  restartWithSilentToken();
}

void silentRestartToManageFonts() { silentRestartToNetwork(NetworkBootTarget::MANAGE_FONTS); }

namespace {
// Reader work after a Wi-Fi session needs internal RAM for worker task stacks
// (24 KB each) and inline image decoding; the same bar as optional rebuilds.
KNOB_ALIAS(NETWORK_EXIT_IN_PLACE_MIN_INTERNAL_BLOCK, netExitMinBlock);  // Goodies > Knobs
// Going Home on a PSRAM device: reader worker stacks and image decoders use
// PSRAM, leaving text layout's 32 KB bar. Free blocks come in 2 KB steps less a
// 12 B header, so a "32 KB" block reads 32756; 31 KB accepts it.
constexpr uint32_t NETWORK_EXIT_HOME_MIN_INTERNAL_BLOCK = MemoryBudget::EPUB_TEXT_LAYOUT_MIN_MAX_ALLOC - 1024;
bool readerResourcesReady = false;
// The render task has the reader's stack (not the 8 KB network-boot one).
bool readerRenderStackReady = false;
}  // namespace

#ifndef SIMULATOR
#if CROSSDINK_PERF_LOG
// Debug: a map of internal RAM's large free runs and the used blocks on each
// side of them, i.e. what bounds the block the Wi-Fi exit gate needs (a block
// between two runs, or one that grew into a region's free tail). The walker
// runs under the heap lock, so it only records; the log comes after.
struct HeapPinScan {
  static constexpr size_t MAX_RUNS = 8;
  static constexpr size_t MIN_FREE_RUN = 4096;
  struct Block {
    uintptr_t addr;
    uint32_t size;
  };
  struct Run {
    intptr_t regionStart;
    intptr_t regionEnd;
    uintptr_t addr;
    uint32_t size;
    Block before;  // {0, 0}: region start
    Block after;   // {0, 0}: region end
  };
  Run runs[MAX_RUNS];
  size_t count = 0;
  Run current{};
  bool inFree = false;
  Block lastUsed{};

  void closeRun(const Block after) {
    if (inFree && current.size >= MIN_FREE_RUN && count < MAX_RUNS) {
      current.after = after;
      runs[count++] = current;
    }
    inFree = false;
  }
};

static bool heapPinWalker(walker_heap_into_t heap, walker_block_info_t block, void* user) {
  auto& scan = *static_cast<HeapPinScan*>(user);
  if (heap.start != scan.current.regionStart) {
    scan.closeRun({});
    scan.current.regionStart = heap.start;
    scan.current.regionEnd = heap.end;
    scan.lastUsed = {};
  }
  const HeapPinScan::Block here{reinterpret_cast<uintptr_t>(block.ptr), static_cast<uint32_t>(block.size)};
  if (block.used) {
    scan.closeRun(here);
    scan.lastUsed = here;
  } else {
    if (!scan.inFree) {
      scan.inFree = true;
      scan.current.addr = here.addr;
      scan.current.size = 0;
      scan.current.before = scan.lastUsed;
    }
    scan.current.size += here.size;
  }
  return true;
}

// Heap poisoning (light) puts a canary and the requested size before the data.
struct BlockPeek {
  uint32_t size;  // requested bytes, 0 when there is no poisoning header
  uint32_t words[2];
};
static BlockPeek peekBlock(const uintptr_t addr) {
  constexpr uint32_t HEAD_CANARY = 0xABBA1234;
  BlockPeek peek{};
  if (addr == 0) return peek;
  uint32_t head[2];
  memcpy(head, reinterpret_cast<const void*>(addr), sizeof(head));
  const bool poisoned = head[0] == HEAD_CANARY;
  if (poisoned) peek.size = head[1];
  memcpy(peek.words, reinterpret_cast<const void*>(addr + (poisoned ? sizeof(head) : 0)), sizeof(peek.words));
  return peek;
}

// Each neighbour names the task whose TCB or stack it is; otherwise its first
// data words hint at the owner (a vtable, a pcb, a string).
void logInternalHeapPins(const char* why) {
  static HeapPinScan scan;  // the walker runs under the heap lock: no allocation
  static TaskStatus_t tasks[40];
  scan = HeapPinScan{};
  heap_caps_walk(MALLOC_CAP_INTERNAL, heapPinWalker, &scan);
  scan.closeRun({});
  const UBaseType_t taskCount = uxTaskGetSystemState(tasks, 40, nullptr);
  LOG_INF("HEAP", "%s: internal free %u largest %u", why,
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
  const auto taskIn = [&](const HeapPinScan::Block& b) -> const char* {
    for (UBaseType_t t = 0; t < taskCount; ++t) {
      const auto tcb = reinterpret_cast<uintptr_t>(tasks[t].xHandle);
      const auto stack = reinterpret_cast<uintptr_t>(tasks[t].pxStackBase);
      if ((tcb >= b.addr && tcb < b.addr + b.size) || (stack >= b.addr && stack < b.addr + b.size)) {
        return tasks[t].pcTaskName;
      }
    }
    return "-";
  };
  for (size_t i = 0; i < scan.count; ++i) {
    const auto& run = scan.runs[i];
    const BlockPeek before = peekBlock(run.before.addr);
    const BlockPeek after = peekBlock(run.after.addr);
    LOG_INF("HEAP",
            "free 0x%08x %u B in 0x%08x-0x%08x | before 0x%08x %u B (%u) %s %08x %08x | after 0x%08x %u B (%u) %s "
            "%08x %08x",
            static_cast<unsigned>(run.addr), static_cast<unsigned>(run.size), static_cast<unsigned>(run.regionStart),
            static_cast<unsigned>(run.regionEnd), static_cast<unsigned>(run.before.addr),
            static_cast<unsigned>(run.before.size), static_cast<unsigned>(before.size), taskIn(run.before),
            static_cast<unsigned>(before.words[0]), static_cast<unsigned>(before.words[1]),
            static_cast<unsigned>(run.after.addr), static_cast<unsigned>(run.after.size),
            static_cast<unsigned>(after.size), taskIn(run.after), static_cast<unsigned>(after.words[0]),
            static_cast<unsigned>(after.words[1]));
  }
  if (scan.count == 0) {
    LOG_INF("HEAP", "no free runs >= %u B", static_cast<unsigned>(HeapPinScan::MIN_FREE_RUN));
  }
}
#endif
#endif
#if !CROSSDINK_PERF_LOG || defined(SIMULATOR)
void logInternalHeapPins(const char*) {}
#endif

bool keepWifiForRemote() {
#if CROSSDINK_GOODIES
  return goodies_remote::keepsStation();
#else
  return false;
#endif
}

// Set while a Wi-Fi screen hands over to another through NetworkEntryActivity,
// whose entry check guards the heap that screen runs on.
static bool networkEntryPending = false;

bool leaveNetworkInPlace(const bool goingHome) {
  if (deepSleepInProgress) return true;
#ifndef SIMULATOR
  const wifi_mode_t mode = WiFi.getMode();
  // The Goodies remote's own network: hand the link back, no teardown and rejoin.
  const bool keepLink = keepWifiForRemote();
  if (mode != WIFI_MODE_NULL) {
    if (!keepLink && (mode & WIFI_MODE_AP)) WiFi.softAPdisconnect(true);
    if (!keepLink && (mode & WIFI_MODE_STA)) WiFi.disconnect(true);
    // Arduino keeps the power-save mode across sessions: a KOSync or Nearby
    // WiFi.setSleep(false) otherwise leaves every later session (OPDS
    // browsing: wifi lock 100%) without modem sleep.
    WiFi.setSleep(true);
    // WIFI_OFF stops the driver and calls esp_wifi_deinit(), returning its
    // internal buffers before the heap check below.
    if (!keepLink) WiFi.mode(WIFI_OFF);
  }
  if (!readerRenderStackReady) {
    LOG_INF("MAIN", "Leaving Wi-Fi by restart: render task has the network-boot stack");
    return false;
  }
  // Remote kept the link and the next screen is another Wi-Fi screen: nothing
  // was torn down, and NetworkEntryActivity re-checks the heap (falling back to
  // a restart into that screen, not Home).
  if (keepLink && networkEntryPending) {
    LOG_INF("MAIN", "Leaving Wi-Fi in place: remote keeps the link for the next Wi-Fi screen");
    return true;
  }
  logInternalHeapPins("Wi-Fi exit");
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  // The Goodies remote rejoins with a 16 KB block (wifiRemoteMinBlock), under Home's bar.
  const uint32_t need = goingHome && psramHeapAvailable() ? NETWORK_EXIT_HOME_MIN_INTERNAL_BLOCK
                                                          : NETWORK_EXIT_IN_PLACE_MIN_INTERNAL_BLOCK;
  if (largest < need) {
    LOG_INF("MAIN", "Leaving Wi-Fi by restart: internal largest block %u < %u", static_cast<unsigned>(largest),
            static_cast<unsigned>(need));
    return false;
  }
  LOG_INF("MAIN", "Leaving Wi-Fi in place: internal largest block %u, free %u", static_cast<unsigned>(largest),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
#endif
  if (!readerResourcesReady) {
    // What a minimal network boot skipped (setupDisplayAndFonts, setup()).
    sdFontSystem.begin(renderer);
    Dictionary::isValidDictionary();
    readerResourcesReady = true;
  }
  return true;
}

static bool networkExitPending = false;
static std::string networkExitBook;

void leaveNetworkAfterExit(std::string bookPath) {
  networkExitPending = true;
  networkExitBook = std::move(bookPath);
}

void finishNetworkExit() {
  if (!networkExitPending) return;
  networkExitPending = false;
  std::string book = std::move(networkExitBook);
  if (leaveNetworkInPlace(/*goingHome=*/book.empty())) return;
  if (book.empty()) {
    silentRestart();
    return;
  }
  // goToReader() is lost across the reboot: reopen the book from APP_STATE.
  APP_STATE.openEpubPath = std::move(book);
  APP_STATE.saveToFile();
  silentRestartToReader();
}

static bool launchNetworkTarget(NetworkBootTarget target, uint32_t payload, bool inPlace);

namespace {
// Wi-Fi uses internal RAM for driver state and buffers that PSRAM cannot hold;
// below this the screen is entered through a reboot as before.
KNOB_ALIAS(NETWORK_ENTRY_IN_PLACE_MIN_INTERNAL_FREE, netEntryMinFree);  // Goodies > Knobs
KNOB_ALIAS(NETWORK_ENTRY_IN_PLACE_MIN_INTERNAL_BLOCK, netEntryMinBlock);

bool enterNetworkInPlace() {
  // The previous activity (a reader included) has run onExit() by now.
  sdFontSystem.releaseForNetwork(renderer);
  ImageBlock::releaseSessionPixelCache();
#ifndef SIMULATOR
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (free < NETWORK_ENTRY_IN_PLACE_MIN_INTERNAL_FREE || largest < NETWORK_ENTRY_IN_PLACE_MIN_INTERNAL_BLOCK) {
    LOG_INF("MAIN", "Entering Wi-Fi by restart: internal free %u largest %u", static_cast<unsigned>(free),
            static_cast<unsigned>(largest));
    return false;
  }
  LOG_INF("MAIN", "Entering Wi-Fi in place: internal free %u largest %u", static_cast<unsigned>(free),
          static_cast<unsigned>(largest));
#endif
  return true;
}

// Replaces the current screen first, so its onExit() frees reader state before
// the heap check; then opens the Wi-Fi screen in place or reboots into it.
class NetworkEntryActivity final : public Activity {
 public:
  NetworkEntryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const NetworkBootTarget target,
                       const uint32_t payload)
      : Activity("NetworkEntry", renderer, mappedInput), target_(target), payload_(payload) {}

  void onEnter() override {
    Activity::onEnter();
    networkEntryPending = false;
    if (enterNetworkInPlace() && launchNetworkTarget(target_, payload_, /*inPlace=*/true)) return;
    restartToNetworkTarget(target_, payload_);
  }

  void render(RenderLock&&) override { GUI.drawPopup(renderer, tr(STR_LOADING_POPUP)); }
  bool usesWifi() const override { return true; }

 private:
  NetworkBootTarget target_;
  uint32_t payload_;
};
}  // namespace

void silentRestartToNetwork(const NetworkBootTarget target, const uint32_t payload) {
  if (deepSleepInProgress) return;
  activityManager.cancelOptionalRenderWork("network");
  auto entry = makeUniqueNoThrow<NetworkEntryActivity>(renderer, mappedInputManager, target, payload);
  if (!entry) {
    restartToNetworkTarget(target, payload);
    return;
  }
  networkEntryPending = true;
  activityManager.replaceActivity(std::move(entry));
}

void silentRestartToFirmwareUpdate(const std::string& firmwarePath) {
  if (!firmwarePath.empty() && firmwarePath.size() < MAX_SILENT_FIRMWARE_PATH) {
    memcpy(silentFirmwareUpdatePath, firmwarePath.c_str(), firmwarePath.size() + 1);
    silentFirmwareUpdateMagic = SILENT_FIRMWARE_UPDATE_MAGIC;
  }
  silentRestartToHome(0, "target=home+firmware-update");
  // Only reached when deep sleep superseded the reboot; don't prompt on wake.
  silentFirmwareUpdateMagic = 0;
}

std::string consumeSilentRestartFirmwareUpdate() {
  std::string path;
  if (silentFirmwareUpdateMagic == SILENT_FIRMWARE_UPDATE_MAGIC) {
    silentFirmwareUpdatePath[MAX_SILENT_FIRMWARE_PATH - 1] = '\0';
    path = silentFirmwareUpdatePath;
  }
  silentFirmwareUpdateMagic = 0;
  return path;
}

static uint32_t encodeKOReaderSyncOrientation(const uint8_t orientation) {
  return orientation < CrossPointSettings::ORIENTATION_COUNT ? static_cast<uint32_t>(orientation) + 1 : 0;
}

static uint8_t decodeKOReaderSyncOrientation(const uint32_t payload) {
  return payload > 0 && payload <= CrossPointSettings::ORIENTATION_COUNT ? static_cast<uint8_t>(payload - 1)
                                                                         : CrossPointSettings::ORIENTATION_COUNT;
}

bool isGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action) {
  return isPowerButtonActionAvailableOutsideReader(action);
}

bool startGlobalSyncProgress(const bool networkBootReady, const uint8_t readerOrientation) {
  if (activityManager.hasActivityNamed(KOReaderSyncActivity::NAME)) {
    LOG_DBG("MAIN", "Ignoring KOReader sync shortcut while sync is already active");
    return true;
  }

  if (!KOREADER_STORE.hasCredentials()) {
    if (networkBootReady) return false;
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  std::string epubPath = APP_STATE.openEpubPath;
  if (epubPath.empty() || !FsHelpers::hasEpubExtension(epubPath) || !Storage.exists(epubPath.c_str())) {
    if (networkBootReady) return false;
    LOG_DBG("MAIN", "No syncable EPUB open, opening KOReader settings instead");
    activityManager.pushActivity(std::make_unique<KOReaderSettingsActivity>(renderer, mappedInputManager));
    return true;
  }

  if (!networkBootReady) {
    silentRestartToNetwork(NetworkBootTarget::KOREADER_SYNC, encodeKOReaderSyncOrientation(readerOrientation));
    return true;
  }

  const DocumentMatchMethod matchMethod = KOREADER_STORE.getMatchMethod();
  auto syncActivity = makeUniqueNoThrow<KOReaderSyncActivity>(renderer, mappedInputManager, std::move(epubPath),
                                                              matchMethod, readerOrientation);
  if (!syncActivity) {
    LOG_ERR("MAIN", "OOM: KOReader sync activity (free=%" PRIu32 " maxAlloc=%" PRIu32 ")", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
    return false;
  }
  activityManager.replaceActivity(std::move(syncActivity));
  return true;
}

// Opens a Wi-Fi screen the way a minimal network boot does. inPlace: entered
// without the reboot, so the stores a network boot loads are loaded here.
static bool launchNetworkTarget(const NetworkBootTarget target, const uint32_t payload, const bool inPlace) {
  if (inPlace && (target == NetworkBootTarget::KOREADER_SYNC || target == NetworkBootTarget::KOREADER_AUTH ||
                  target == NetworkBootTarget::FILE_TRANSFER)) {
    KOREADER_STORE.loadFromFile();
  }
  bool launched = false;
  switch (target) {
    case NetworkBootTarget::OTA: {
      auto otaActivity = makeUniqueNoThrow<OtaUpdateActivity>(renderer, mappedInputManager);
      if (otaActivity) {
        activityManager.replaceActivity(std::move(otaActivity));
        launched = true;
      } else {
        LOG_ERR("MAIN", "OOM: OTA activity after minimal boot (free=%" PRIu32 " maxAlloc=%" PRIu32 ")",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      break;
    }
    case NetworkBootTarget::OPDS:
      launched = activityManager.goToOpdsServer(payload, true);
      break;
    case NetworkBootTarget::KOREADER_SYNC:
      launched = startGlobalSyncProgress(true, decodeKOReaderSyncOrientation(payload));
      break;
    case NetworkBootTarget::KOREADER_AUTH: {
      const auto mode = payload == 1 ? KOReaderAuthActivity::Mode::SIGN_UP : KOReaderAuthActivity::Mode::AUTHENTICATE;
      auto authActivity = makeUniqueNoThrow<KOReaderAuthActivity>(renderer, mappedInputManager, mode);
      if (authActivity) {
        activityManager.replaceActivity(std::move(authActivity));
        launched = true;
      } else {
        LOG_ERR("MAIN", "OOM: KOReader auth activity after minimal boot (free=%" PRIu32 " maxAlloc=%" PRIu32 ")",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      break;
    }
    case NetworkBootTarget::FILE_TRANSFER:
      launched = activityManager.resumeFileTransferFromNetworkBoot(payload);
      break;
    case NetworkBootTarget::MANAGE_FONTS: {
      auto fontsActivity = makeUniqueNoThrow<FontDownloadActivity>(renderer, mappedInputManager);
      if (fontsActivity) {
        activityManager.replaceActivity(std::move(fontsActivity));
        launched = true;
      } else {
        LOG_ERR("MAIN", "OOM: Manage Fonts activity after minimal boot (free=%" PRIu32 " maxAlloc=%" PRIu32 ")",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
      break;
    }
  }
  return launched;
}

CrossPointSettings::SHORT_PWRBTN getPowerButtonAction() {
  static bool longPowerButtonHandled = false;

  if (mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    if (longPowerButtonHandled) {
      longPowerButtonHandled = false;
      return CrossPointSettings::SHORT_PWRBTN::IGNORE;
    }

    return mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()
               ? static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.shortPwrBtn)
               : static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  }

  if (longPowerButtonHandled || !mappedInputManager.isPressed(MappedInputManager::Button::Power) ||
      mappedInputManager.getHeldTime() < SETTINGS.getPowerButtonLongPressDuration()) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  const auto action = static_cast<CrossPointSettings::SHORT_PWRBTN>(SETTINGS.longPwrBtn);
  if (!isGlobalPowerButtonAction(action)) {
    return CrossPointSettings::SHORT_PWRBTN::IGNORE;
  }

  longPowerButtonHandled = true;
  return action;
}

void notifyQuickLockChanged() {
  const bool locked = buttonShortcutController.isQuickLocked();
  x4ProHomeKeyTapPending = false;
  mappedInputManager.clearInjectedReleases();
  LOG_DBG("MAIN", "Quick Lock %s", locked ? "enabled" : "disabled");
  if (locked) {
    APP_STATE.quickLockRestoreFrontlight = Frontlight.isOn();
    Frontlight.setOn(false);
    BatteryLog::lightChanged();
    activityManager.notifyInputLockChanged(true);
    int top = 0;
    int right = 0;
    int bottom = 0;
    int left = 0;
    renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
    const int x = std::max(left, renderer.getScreenWidth() - right - QuickLockBadgeBackdrop::SIZE);
    const int y = std::max(top, renderer.getScreenHeight() - bottom - QuickLockBadgeBackdrop::SIZE);
    // ActivityManager applies Night Mode at the display boundary, so direct
    // framebuffer writes retain the normal palette.
    constexpr bool background = false;
    constexpr bool foreground = true;
    RenderLock lock;
    quickLockBadgeBackdrop.save(renderer, x, y);
    renderer.fillRect(x, y, QuickLockBadgeBackdrop::SIZE, QuickLockBadgeBackdrop::SIZE, background);
    freeink::ui::GfxRendererTarget target(renderer);
    target.bitmap(freeink::ui::Rect{x + (QuickLockBadgeBackdrop::SIZE - icon_tabler_lock_28.w) / 2,
                                    y + (QuickLockBadgeBackdrop::SIZE - icon_tabler_lock_28.h) / 2,
                                    icon_tabler_lock_28.w, icon_tabler_lock_28.h},
                  freeink::ui::bitmapFromIcon(icon_tabler_lock_28), freeink::ui::BitmapMode::Center,
                  freeink::ui::Paint::solid(foreground ? freeink::ui::Color::Black : freeink::ui::Color::White));
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  } else {
    bool restoredBadgeBackdrop = false;
    {
      RenderLock lock;
      restoredBadgeBackdrop = quickLockBadgeBackdrop.restore(renderer);
      if (restoredBadgeBackdrop) renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    }
    if (APP_STATE.quickLockRestoreFrontlight) {
      Frontlight.setOn(true);
      BatteryLog::lightChanged();
      APP_STATE.quickLockRestoreFrontlight = false;
    }
    if (!restoredBadgeBackdrop) (void)activityManager.requestUpdateAndWait();
    activityManager.notifyInputLockChanged(false);
  }
}

bool handleGlobalPowerButtonAction(const CrossPointSettings::SHORT_PWRBTN action,
                                   const QuickLockTrigger quickLockTrigger) {
  switch (action) {
    case CrossPointSettings::SHORT_PWRBTN::SLEEP:
    case CrossPointSettings::SHORT_PWRBTN::SLEEP_ONLY:
      enterDeepSleep();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::WAKE_ONLY:
      return true;
    case CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK:
      if (quickLockTrigger == QuickLockTrigger::None) {
        LOG_ERR("MAIN", "Quick Lock requested without an input trigger");
        return false;
      }
      buttonShortcutController.toggleQuickLock(millis(), quickLockTrigger,
                                               quickLockTrigger == QuickLockTrigger::LongPower);
      notifyQuickLockChanged();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH: {
      // Reader redraws must replace overlays before the panel refreshes.
      if (activityManager.requestManualReaderRefresh()) {
        return true;
      }
      RenderLock lock;
      renderer.displayBuffer(manualScreenRefreshMode());
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::SCREENSHOT: {
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      return startGlobalSyncProgress();
    case CrossPointSettings::SHORT_PWRBTN::FILE_TRANSFER:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToFileTransfer();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CALIBRE_WIRELESS:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToCalibreWireless();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::JOIN_NETWORK:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToJoinNetworkFileTransfer();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::CREATE_HOTSPOT:
      if (activityManager.canSnapshotForSleepOverlay()) {
        return false;
      }
      activityManager.goToHotspotFileTransfer();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::LIBRARY:
      activityManager.goToLibrary();
      return true;
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_FRONTLIGHT: {
      if (!Frontlight.present()) return false;
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      activityManager.persistGlobalSettings();
      LOG_INF("LIGHT", "Frontlight toggled %s by shortcut", lightOn ? "on" : "off");
      return true;
    }
    case CrossPointSettings::SHORT_PWRBTN::TOGGLE_TOUCHSCREEN:
      if (!gpio.hasTouch()) return false;
      SETTINGS.disableReaderTouchscreen = SETTINGS.disableReaderTouchscreen ? 0 : 1;
      activityManager.persistGlobalSettings();
      LOG_INF("TOUCH", "Reader touchscreen %s by shortcut", SETTINGS.disableReaderTouchscreen ? "disabled" : "enabled");
      {
        RenderLock lock;
        BookActions::drawToast(
            renderer, SETTINGS.disableReaderTouchscreen ? tr(STR_TOUCHSCREEN_DISABLED) : tr(STR_TOUCHSCREEN_ENABLED));
      }
      delay(1000);
      activityManager.requestUpdate();
      return true;
    default:
      return false;
  }
  return false;
}

bool dispatchShortcutAction(const CrossPointSettings::SHORT_PWRBTN action) {
  if (action == CrossPointSettings::SHORT_PWRBTN::READING_STATS && !SETTINGS.shouldTrackReadingStats()) return false;
  // An EPUB reader may have a per-book orientation that is restored during
  // teardown. Let it hand off Sync Progress before the global restart drops
  // that transient setting.
  if (action == CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS && activityManager.handleShortcutAction(action)) {
    return true;
  }
  return handleGlobalPowerButtonAction(action) || activityManager.handleShortcutAction(action);
}

ButtonShortcutController::ChordAction configuredChordAction() {
  const auto rawAction = SETTINGS.powerChordAction;
  if (rawAction >= CrossPointSettings::POWER_CHORD_ACTION_COUNT) {
    return ButtonShortcutController::ChordAction::Disabled;
  }
  return static_cast<ButtonShortcutController::ChordAction>(rawAction);
}

ButtonShortcutController::ChordAction configuredSideButtonChordAction() {
  const auto rawAction = SETTINGS.sideButtonChordAction;
  if (rawAction >= CrossPointSettings::POWER_CHORD_ACTION_COUNT) {
    return ButtonShortcutController::ChordAction::Disabled;
  }
  return static_cast<ButtonShortcutController::ChordAction>(rawAction);
}

CrossPointSettings::SHORT_PWRBTN chordPowerAction(const ButtonShortcutController::ChordAction action) {
  using Chord = ButtonShortcutController::ChordAction;
  using Power = CrossPointSettings::SHORT_PWRBTN;
  switch (action) {
    case Chord::Sleep:
      return Power::SLEEP;
    case Chord::PageTurn:
      return Power::PAGE_TURN;
    case Chord::PreviousPage:
      return Power::PREVIOUS_PAGE;
    case Chord::ToggleBookmark:
      return Power::TOGGLE_BOOKMARK;
    case Chord::ReadingStats:
      return Power::READING_STATS;
    case Chord::MarkFinished:
      return Power::MARK_FINISHED;
    case Chord::ForceRefresh:
      return Power::FORCE_REFRESH;
    case Chord::ToggleFont:
      return Power::TOGGLE_FONT;
    case Chord::ToggleGuideDots:
      return Power::TOGGLE_GUIDE_DOTS;
    case Chord::ToggleFocusReading:
      return Power::TOGGLE_FOCUS_READING;
    case Chord::CyclePageTurn:
      return Power::CYCLE_PAGE_TURN;
    case Chord::SyncProgress:
      return Power::SYNC_PROGRESS;
    case Chord::NearbyPositionSync:
      return Power::NEARBY_POSITION_SYNC;
    case Chord::Library:
      return Power::LIBRARY;
    case Chord::FileTransfer:
      return Power::FILE_TRANSFER;
    case Chord::CalibreWireless:
      return Power::CALIBRE_WIRELESS;
    case Chord::JoinNetwork:
      return Power::JOIN_NETWORK;
    case Chord::CreateHotspot:
      return Power::CREATE_HOTSPOT;
    case Chord::ToggleDarkMode:
      return Power::TOGGLE_DARK_MODE;
    case Chord::Footnotes:
      return Power::FOOTNOTES;
    case Chord::FileBrowser:
      return Power::FILE_BROWSER;
    case Chord::CreateClipping:
      return Power::CREATE_CLIPPING;
    case Chord::LookupWord:
      return Power::LOOKUP_WORD;
    case Chord::ToggleHomeButton:
      return Power::TOGGLE_HOME_BUTTON_IN_READER;
    case Chord::QuickActions:
      return Power::QUICK_ACTIONS;
    case Chord::ToggleFrontlight:
      return Power::TOGGLE_FRONTLIGHT;
    case Chord::ToggleTouchscreen:
      return Power::TOGGLE_TOUCHSCREEN;
    default:
      return Power::IGNORE;
  }
}

bool dispatchButtonShortcut(const ButtonShortcutController::Result& result) {
  switch (result.event) {
    case ButtonShortcutController::Event::None:
      return false;
    case ButtonShortcutController::Event::QuickLockChanged:
      notifyQuickLockChanged();
      return true;
    case ButtonShortcutController::Event::Screenshot: {
      RenderLock lock;
      ScreenshotUtil::takeScreenshot(renderer);
      return true;
    }
    case ButtonShortcutController::Event::PageTurn:
      mappedInputManager.injectRelease(MappedInputManager::Button::Right);
      break;
    case ButtonShortcutController::Event::ConfiguredAction:
      return dispatchShortcutAction(chordPowerAction(result.action));
    case ButtonShortcutController::Event::TouchscreenEscapeHatch:
      return activityManager.openReaderSettingsForTouchscreenEscapeHatch();
  }

  activityManager.loop();
  mappedInputManager.clearInjectedReleases();
  return true;
}

namespace {
constexpr uint8_t TILT_SLEEP_MAX_ATTEMPTS = 3;
constexpr uint16_t TILT_SLEEP_RETRY_DELAY_MS = 10;

void putTiltSensorToSleepForDeepSleep() {
  if (!halTiltSensor.isAvailable()) {
    return;
  }

  for (uint8_t attempt = 0; attempt < TILT_SLEEP_MAX_ATTEMPTS; ++attempt) {
    if (halTiltSensor.deepSleep()) {
      return;
    }
    delay(TILT_SLEEP_RETRY_DELAY_MS);
  }
  LOG_ERR("MAIN", "Tilt sensor did not confirm sleep before deep sleep");
}

bool executeX4ProHomeButtonAction(const uint8_t action,
                                  const QuickLockTrigger quickLockTrigger = QuickLockTrigger::None) {
  switch (action) {
    case CrossPointSettings::HOME_BUTTON_BACK_HOME:
      return activityManager.handleHomeButtonBackOrHome();
    case CrossPointSettings::HOME_BUTTON_TOGGLE_FRONTLIGHT: {
      const bool lightOn = !Frontlight.isOn();
      Frontlight.setOn(lightOn);
      SETTINGS.frontlightOn = lightOn ? 1 : 0;
      activityManager.persistGlobalSettings();
      LOG_INF("LIGHT", "Frontlight toggled %s by Home key", lightOn ? "on" : "off");
      return true;
    }
    case CrossPointSettings::HOME_BUTTON_READER_MENU:
      return activityManager.openReaderMenuFromShortcut();
    default:
      break;
  }

  if (action >= CrossPointSettings::SHORT_PWRBTN_COUNT) {
    return false;
  }

  const auto powerAction = static_cast<CrossPointSettings::SHORT_PWRBTN>(action);
  if (powerAction == CrossPointSettings::SHORT_PWRBTN::SYNC_PROGRESS) {
    dispatchShortcutAction(powerAction);
    return true;
  }
  if (handleGlobalPowerButtonAction(powerAction, quickLockTrigger)) {
    return true;
  }
  activityManager.handleShortcutAction(powerAction);
  return true;
}

bool wasX4ProHomeKeyTapped() {
#ifdef SIMULATOR
  return simulatorHomeKeyInput.wasTapped();
#else
  return gpio.wasHomeKeyTapped();
#endif
}

bool wasX4ProHomeKeyLongPressed() {
#ifdef SIMULATOR
  return simulatorHomeKeyInput.wasLongPressed();
#else
  return gpio.wasHomeKeyLongPressed();
#endif
}

bool unlockX4ProHomeQuickLock(const QuickLockTrigger trigger) {
  if (!buttonShortcutController.tryUnlockWithTrigger(millis(), trigger)) {
    return false;
  }
  notifyQuickLockChanged();
  return true;
}

bool handleX4ProHomeKeyQuickLockUnlock() {
  if (!mappedInputManager.hasHomeKey()) {
    return false;
  }

  const QuickLockTrigger trigger = buttonShortcutController.quickLockTrigger();
  if (trigger == QuickLockTrigger::HomeTap) {
    return wasX4ProHomeKeyTapped() && unlockX4ProHomeQuickLock(trigger);
  }
  if (trigger == QuickLockTrigger::HomeLongPress) {
    return wasX4ProHomeKeyLongPressed() && unlockX4ProHomeQuickLock(trigger);
  }
  if (trigger != QuickLockTrigger::HomeDoubleTap) {
    return false;
  }

  const unsigned long now = millis();
  if (x4ProHomeKeyTapPending && now - lastX4ProHomeKeyTapAt > X4PRO_HOME_KEY_DOUBLE_TAP_MS) {
    x4ProHomeKeyTapPending = false;
  }
  if (!wasX4ProHomeKeyTapped()) {
    return false;
  }
  if (!x4ProHomeKeyTapPending) {
    lastX4ProHomeKeyTapAt = now;
    x4ProHomeKeyTapPending = true;
    return true;
  }

  x4ProHomeKeyTapPending = false;
  return unlockX4ProHomeQuickLock(trigger);
}

bool handleX4ProHomeKeyShortcuts() {
  if (!mappedInputManager.hasHomeKey()) {
    return false;
  }

  // A modal owns Home too. Clear a pending single tap so a gesture started
  // while Quick Actions is open cannot fire after the popup closes.
  if (activityManager.blocksGlobalInput()) {
    const bool hadPendingTap = x4ProHomeKeyTapPending;
    x4ProHomeKeyTapPending = false;
    mappedInputManager.clearDeferredHomeGesture();
    return hadPendingTap || wasX4ProHomeKeyTapped() || wasX4ProHomeKeyLongPressed();
  }

  // Reader menus set the touchscreen override while they are active, which
  // intentionally lets Home work there. On a page, consume every Home edge
  // and discard a deferred single tap so nothing fires after it is re-enabled.
  if (mappedInputManager.isHomeButtonLockedInReader()) {
    const bool hadPendingTap = x4ProHomeKeyTapPending;
    x4ProHomeKeyTapPending = false;
    mappedInputManager.clearDeferredHomeGesture();
    return hadPendingTap || wasX4ProHomeKeyTapped() || wasX4ProHomeKeyLongPressed();
  }

  // A lower-bezel swipe can report a capacitive Home tap as well. Let the
  // screen gesture win and cancel any delayed single-tap interpretation so it
  // cannot navigate Home after the list has already handled the swipe.
  if (mappedInputManager.wasSwipe() != MappedInputManager::SwipeDir::None) {
    x4ProHomeKeyTapPending = false;
    mappedInputManager.clearDeferredHomeGesture();
    return false;
  }

  const unsigned long now = millis();
  bool completedPendingTap = false;
  if (x4ProHomeKeyTapPending && now - lastX4ProHomeKeyTapAt > X4PRO_HOME_KEY_DOUBLE_TAP_MS) {
    // A single-tap action must wait briefly so the reader does not navigate
    // away before a second capacitive-key tap can be recognized.
    x4ProHomeKeyTapPending = false;
    if (SETTINGS.homeButtonTapAction == CrossPointSettings::HOME_BUTTON_BACK_HOME) {
      // Keep reader menus and other overlays on their existing local Home route.
      mappedInputManager.queueDeferredHomeGesture();
    } else {
      executeX4ProHomeButtonAction(SETTINGS.homeButtonTapAction, QuickLockTrigger::HomeTap);
      completedPendingTap = true;
    }
  }

  if (wasX4ProHomeKeyLongPressed()) {
    // A hold is a separate gesture, not the second half of a double tap.
    x4ProHomeKeyTapPending = false;
    if (SETTINGS.homeButtonLongPressAction == CrossPointSettings::HOME_BUTTON_READER_MENU) {
      return completedPendingTap;
    }
    executeX4ProHomeButtonAction(SETTINGS.homeButtonLongPressAction, QuickLockTrigger::HomeLongPress);
    return true;
  }

  if (!wasX4ProHomeKeyTapped()) return completedPendingTap;

  // With no double-tap action there is nothing to wait for, so a tap acts at
  // once. Back/Home lets this frame's raw tap reach the activity's own route.
  if (SETTINGS.homeButtonDoubleTapAction == CrossPointSettings::SHORT_PWRBTN::IGNORE) {
    x4ProHomeKeyTapPending = false;
    if (SETTINGS.homeButtonTapAction == CrossPointSettings::HOME_BUTTON_BACK_HOME) return completedPendingTap;
    executeX4ProHomeButtonAction(SETTINGS.homeButtonTapAction, QuickLockTrigger::HomeTap);
    return true;
  }

  if (!x4ProHomeKeyTapPending) {
    lastX4ProHomeKeyTapAt = now;
    x4ProHomeKeyTapPending = true;
    return true;
  }

  x4ProHomeKeyTapPending = false;
  executeX4ProHomeButtonAction(SETTINGS.homeButtonDoubleTapAction, QuickLockTrigger::HomeDoubleTap);
  return true;
}
}  // namespace
constexpr char SLEEP_FRAME_FILE[] = "/.crossdink/sleep_frame.bin";

static void saveSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForWrite("SLP", SLEEP_FRAME_FILE, file)) return;
  file.write(renderer.getFrameBuffer(), renderer.getBufferSize());
  file.close();
}

static bool loadSleepFrameBuffer() {
  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) return false;
  const size_t bufferSize = display.getBufferSize();
  const size_t bytesRead = file.read(display.getFrameBuffer(), bufferSize);
  file.close();
  if (bytesRead != bufferSize) {
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }
  Storage.remove(SLEEP_FRAME_FILE);
  return true;
}

static bool preflightSleepFrameBuffer() {
  if (!Storage.exists(SLEEP_FRAME_FILE)) return false;

  HalFile file;
  if (!Storage.openFileForRead("SLP", SLEEP_FRAME_FILE, file)) {
    LOG_ERR("SLP", "Failed to open Quick Resume frame for preflight");
    Storage.remove(SLEEP_FRAME_FILE);
    return false;
  }

#ifdef SIMULATOR
  // The simulator's legacy display stub has no X3 runtime geometry. This path
  // is unreachable there because it cannot identify a UC8279 X3 profile.
  const size_t expectedSize = HalDisplay::BUFFER_SIZE;
#else
  const size_t expectedSize = EInkDisplay::X3_BUFFER_SIZE;
#endif
  const size_t actualSize = file.fileSize();
  file.close();
  if (SleepWakePolicy::hasValidSavedFrame(true, actualSize, expectedSize)) return true;

  LOG_ERR("SLP", "Invalid Quick Resume frame: expected=%u actual=%u", static_cast<unsigned>(expectedSize),
          static_cast<unsigned>(actualSize));
  Storage.remove(SLEEP_FRAME_FILE);
  return false;
}

bool shouldClearX4WakeGhosting() {
#if FREEINK_DEVICE_X4
  return gpio.deviceIsX4();
#else
  return false;
#endif
}

// Wake validation runs before the SD card and its settings file are available.
// Mirror the one setting that changes its behavior while entering sleep, so a
// permitted short wake press can pass verification even after the button has
// been released during boot. The write is skipped when the value is unchanged.
constexpr char WAKE_NVS_NAMESPACE[] = "crosspoint";
constexpr char WAKE_SHORT_PRESS_KEY[] = "wakeShortPr";

bool readWakeShortPressFromNvs() {
#ifdef SIMULATOR
  return false;
#else
  nvs_handle_t handle;
  if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return false;
  uint8_t value = 0;
  const esp_err_t result = nvs_get_u8(handle, WAKE_SHORT_PRESS_KEY, &value);
  nvs_close(handle);
  return result == ESP_OK && value != 0;
#endif
}

void mirrorWakeShortPressToNvs() {
#ifndef SIMULATOR
  const uint8_t expected = SETTINGS.shortPowerPressWakes() ? 1 : 0;
  nvs_handle_t handle;
  if (nvs_open(WAKE_NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
  uint8_t current = 0;
  const bool hasCurrent = nvs_get_u8(handle, WAKE_SHORT_PRESS_KEY, &current) == ESP_OK;
  if (!hasCurrent || current != expected) {
    nvs_set_u8(handle, WAKE_SHORT_PRESS_KEY, expected);
    nvs_commit(handle);
  }
  nvs_close(handle);
#endif
}

void flushSettingsStores() {
  SETTINGS.flush();
#if CROSSDINK_GOODIES
  knobs::flush();
#endif
}

// Sleep entry has no task watchdog: a wait that never returns leaves the device
// dark and deaf to the power button until a reset (seen once on 1001aa, cause
// unknown). After 30 s, log the step it stuck in (PSRAM ring survives the
// reset) and reset. Idempotent; deep sleep or a restart ends it.
static void armSleepGuard() {
#ifndef SIMULATOR
  static esp_timer_handle_t guard = nullptr;
  if (guard != nullptr) return;
  const esp_timer_create_args_t args = {
      .callback =
          [](void*) {
            // A panic reset skips the shutdown handlers (they could block on the
            // same stuck display), and checkPanic() reports the message on boot.
            static char why[64];
            snprintf(why, sizeof(why), "sleep entry stuck at '%s' for 30 s", HalPowerManager::sleepStep);
            LOG_ERR("SLP", "%s", why);
            esp_system_abort(why);
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "sleepGuard",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &guard) != ESP_OK || esp_timer_start_once(guard, 30 * 1000 * 1000) != ESP_OK) {
    LOG_ERR("SLP", "sleep guard not armed");
  }
#endif
}

// setup() has no watchdog for blocked waits either (SD mount, I2C, a panel BUSY
// wait), and the screen still shows the sleep image, so a hang there looks like
// a wake that never happened. After 60 s, log the last boot phase and reset,
// like the sleep guard above. Disarmed when setup() returns.
static const char* bootStep = "init";
#ifndef SIMULATOR
static esp_timer_handle_t bootGuard = nullptr;
#endif

static void bootPhase(const char* name) {
  bootStep = name;
  PerfLog::noteBootPhase(name);
}

static void armBootGuard() {
#ifndef SIMULATOR
  const esp_timer_create_args_t args = {
      .callback =
          [](void*) {
            static char why[64];
            snprintf(why, sizeof(why), "boot stuck after '%s' for 60 s", bootStep);
            LOG_ERR("BOOT", "%s", why);
            esp_system_abort(why);
          },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "bootGuard",
      .skip_unhandled_events = true,
  };
  if (esp_timer_create(&args, &bootGuard) != ESP_OK || esp_timer_start_once(bootGuard, 60 * 1000 * 1000) != ESP_OK) {
    LOG_ERR("BOOT", "boot guard not armed");
  }
#endif
}

static void disarmBootGuard() {
#ifndef SIMULATOR
  if (bootGuard == nullptr) return;
  esp_timer_stop(bootGuard);
  esp_timer_delete(bootGuard);
  bootGuard = nullptr;
#endif
}

// Before the sleep screen, push under a progress toast and show the result for a
// moment: the open book's just-saved position (KOReader Sync > Sync on Wake &
// Sleep), else a close push still queued or running, which deep sleep would drop.
// pushNow() bounds the whole wait, so sleep always goes on.
void syncBookBeforeSleep() {
  std::string path = kosync_auto::wantsSleepPush() ? activityManager.flushEpubProgressForSync() : std::string();
  const bool readerFlushed = !path.empty();
  if (!readerFlushed) path = kosync_auto::pendingPushPath();
  if (path.empty()) return;
  HalPowerManager::sleepStep = "kosync push";
  activityManager.cancelOptionalRenderWork("kosync sleep push");
  // Held throughout so the reader cannot repaint over the toasts; bounded so a
  // busy render task only costs the toasts, never the push or the sleep.
  RenderLock lock(3000UL);
  // The sleep screen may snapshot this page, so the toasts' band is put back after.
  const int bandH = renderer.getLineHeight(UI_10_FONT_ID) + 24;  // drawToast()'s height
  const int bandY = (renderer.getScreenHeight() - bandH) / 2;
  const int bandW = renderer.getScreenWidth();
  const size_t bandBytes = renderer.getRegionByteSize(0, bandY, bandW, bandH);
  std::unique_ptr<uint8_t[]> band;
  if (lock.ownsLock()) band = makeUniqueNoThrow<uint8_t[]>(bandBytes);
  const bool saved = band && renderer.copyRegionToBuffer(0, bandY, bandW, bandH, band.get(), bandBytes);
  if (saved) BookActions::drawToast(renderer, tr(STR_SYNCING_PROGRESS));
  const kosync_auto::PushOutcome outcome = kosync_auto::pushNow(path, readerFlushed);
  LOG_INF("KOSync", "sleep push outcome %d", static_cast<int>(outcome));
  if (!saved) return;
  const char* msg = outcome == kosync_auto::PushOutcome::Pushed        ? tr(STR_UPLOAD_SUCCESS)
                    : outcome == kosync_auto::PushOutcome::Same        ? tr(STR_ALREADY_SYNCED)
                    : outcome == kosync_auto::PushOutcome::ServerAhead ? tr(STR_SYNC_SERVER_AHEAD)
                                                                       : tr(STR_SYNC_FAILED_MSG);
  BookActions::drawToast(renderer, msg);
  delay(1500);
  renderer.copyBufferToRegion(0, bandY, bandW, bandH, band.get(), bandBytes);
}

// Enter deep sleep mode
void enterDeepSleep(bool fromTimeout) {
  armSleepGuard();
  HalPowerManager::sleepStep = "activity exit";
#if CROSSDINK_GOODIES
  goodies_remote::waitForJoin();  // the Wi-Fi shutdown below must not overlap the remote's join task
#endif
  syncBookBeforeSleep();
  HalPowerManager::sleepStep = "activity exit";
  kosync_auto::yieldRadio();  // likewise auto sync's Wi-Fi start/stop; deep sleep drops the rest
  // Scope the CPU frequency lock so it can be released before deep sleep entry.
  // The lock is held during sleep prep to ensure full speed for file I/O and state
  // save, but it must be released before esp_deep_sleep_start() or the PM system
  // will abort with the lock still held.
  {
    HalPowerManager::Lock powerLock;
    APP_STATE.lastSleepFromReader = activityManager.isReaderActivity();
    // "request" = power button or a Sleep menu/quick action.
    PerfLog::noteDeepSleep(
        fromTimeout ? (APP_STATE.quickLockResumePending ? "quick-lock-timeout" : "idle-timeout") : "request",
        activityManager.currentActivityName());

    const bool isQuickResumeSleep =
        SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::QUICK_RESUME ||
        (fromTimeout &&
         SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT);
    // Every sleep mode leaves a complete retained frame on the e-ink panel. Keep
    // it visible until the first useful reader or Home paint replaces it.
    splashlessWakeMagic = SPLASHLESS_WAKE_MAGIC;

    // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
    // a WiFi activity would otherwise silentRestart() here and reboot instead.
    deepSleepInProgress = true;
    activityManager.goToSleep(fromTimeout);
    wifi_background_join::wait();  // the OPDS list's onExit() may have queued the radio off
    HalPowerManager::sleepStep = "sleep writes";
    ReaderExitSave::flush();  // the reader's exit writes, now behind the sleep screen
    flushSettingsStores();
    // Persist after the sleep screen is up so the write does not delay it. The
    // reader's onExit() usually saves the same state already, so this write is
    // then skipped as unchanged.
    APP_STATE.saveToFile();

    // Sleep screens refresh synchronously and display.deepSleep() waits out any
    // pending refresh and the power-off, so no settle delay is needed here. A
    // delay would only widen the window in which a wake press is swallowed.
    if (isQuickResumeSleep) {
      saveSleepFrameBuffer();
    } else if (Storage.exists(SLEEP_FRAME_FILE)) {
      // A stale Quick Resume frame must not replace the selected sleep screen during wake.
      Storage.remove(SLEEP_FRAME_FILE);
    }

    if (halClock.isAvailable() && SETTINGS.shouldTrackReadingStats() && SETTINGS.autoBackupStats != 0) {
      ReadingStatsDateTime now;
      if (getCurrentLocalReadingStatsDateTime(now) && !backupGlobalStats(false)) {
        LOG_ERR("MAIN", "Automatic reading-stats backup failed before deep sleep");
      }
    }

    // Last chance to sample: startDeepSleep() cuts the SD rail on X3, so nothing
    // can be written again until the next wake.
    BatteryDiagnosticLog::record(BatteryDiagnosticLog::Event::Sleep, BoardConfig::ACTIVE.name);
    BatteryLog::onSleep(fromTimeout ? "idle-timeout" : "request");
    SleepLog::onSleep();
    // All sleep-time file writes are complete. Stop SDMMC before the power path
    // cuts peripheral rails and isolates the bus pads; SPI boards are a no-op.
    HalPowerManager::sleepStep = "storage shutdown";
    Storage.shutdown();

    putTiltSensorToSleepForDeepSleep();
    HalPowerManager::sleepStep = "display sleep";
    display.deepSleep();
    HalPowerManager::sleepStep = "frontlight, nvs";
    Frontlight.prepareForDeepSleep();
    mirrorWakeShortPressToNvs();
    LOG_DBG("MAIN", "Entering deep sleep");

  }  // Release powerLock before deep sleep entry

  SleepLog::restartIfArmed();  // Goodies > Sleep-reboot-log: debug the sleep path without losing the log
  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(const bool seamless, const bool loadReaderResources, const bool useReaderRenderStack) {
#if !defined(SIMULATOR) && !FREEINK_MCU_C3
  // C3 X3/X4 detection already runs in HalGPIO::begin() before SPI owns the
  // panel pins. S3 boards initialize display SPI inside display.begin(), so an
  // X4 Pro must resolve a UC8179 replacement panel here, before driver selection.
  static bool controllerResolved = false;
  if (!controllerResolved) {
    controllerResolved = true;
    freeink::applyXteinkDisplayController();  // DeviceIdentity::logPanel() below reports the outcome
  }
  bootPhase("probe");
#endif

#ifdef SIMULATOR
  (void)seamless;
  display.begin();
#else
  display.begin(seamless);
  esp_register_shutdown_handler(powerOffPanelOnRestart);  // runs before PsramLog's (reverse order)
  // Every restart writes held reader exit data and deferred settings first.
  // One handler: IDF has 5 slots and Wi-Fi takes one.
  esp_register_shutdown_handler([] {
    ReaderExitSave::flush();
    flushSettingsStores();
    BatteryLog::onRestart();
  });
  if (seamless) {
    seedRetainedPanelFrame();
  } else {
    discardRetainedPanelFrame();
  }
  static bool panelLogged = false;
  if (!panelLogged) {
    panelLogged = true;
    DeviceIdentity::logPanel();  // debug builds: exact controller, detect method, VER/MTP
  }
  bootPhase("panel");
#endif
  renderer.begin();
  display.setInverted(SETTINGS.screenInverted != 0);
  // FreeInkUI headers need more than 4 KB once the render loop and nested
  // screen builders share the task stack. Some S3 network flows can render a
  // parent screen before their deferred Wi-Fi child is promoted, so their
  // caller selects the reader-sized stack. Lightweight network targets use
  // 8 KB; reader rendering retains its 16 KB budget.
  activityManager.begin(useReaderRenderStack ? READER_RENDER_TASK_STACK_BYTES : NETWORK_RENDER_TASK_STACK_BYTES);
  readerRenderStackReady = useReaderRenderStack;

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);

#if !CROSSDINK_SCALABLE_FONTS
  renderer.insertFont(LEXENDDECA_10_FONT_ID, lexenddeca10FontFamily);
  renderer.insertFont(LEXENDDECA_12_FONT_ID, lexenddeca12FontFamily);
  renderer.insertFont(LEXENDDECA_14_FONT_ID, lexenddeca14FontFamily);
  renderer.insertFont(LEXENDDECA_16_FONT_ID, lexenddeca16FontFamily);
  renderer.insertFont(BITTER_10_FONT_ID, bitter10FontFamily);
  renderer.insertFont(BITTER_12_FONT_ID, bitter12FontFamily);
  renderer.insertFont(BITTER_14_FONT_ID, bitter14FontFamily);
  renderer.insertFont(BITTER_16_FONT_ID, bitter16FontFamily);
#endif
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  if (loadReaderResources) {
    sdFontSystem.begin(renderer);
    readerResourcesReady = true;
  } else {
    LOG_DBG("MAIN", "Skipping EPUB scratch workspace and SD fonts for minimal network boot");
  }
}

namespace {
void installFlashDuckRenderWait();
}  // namespace

void setup() {
  armBootGuard();
  installFlashDuckRenderWait();
#ifdef SIMULATOR
  SimulatorLifecycle::restoreSilentRebootToken(silentRebootMagic, silentRebootTarget, silentRebootPayload);
#else
  // loopTask shares its core with the priority-1 background workers (library
  // prewarm, OPDS prefetch, dictionary lookup). Run above them so input
  // handling never time-slices with a worker; the loop blocks or sleeps a tick
  // every pass, which is when they run.
  vTaskPrioritySet(nullptr, 2);
#endif
  BoardConfig::holdPowerRails();

  const esp_reset_reason_t rawResetReason = esp_reset_reason();
  const esp_sleep_wakeup_cause_t rawWakeupCause = esp_sleep_get_wakeup_cause();

#ifdef ENABLE_SERIAL_LOG
#ifdef CROSSPOINT_WAIT_FOR_USB_SERIAL
  // Development builds preserve reliable early CDC logs; release builds let
  // enumeration proceed asynchronously so users do not pay this startup cost.
  delay(250);
#endif
  // Web Serial sends file data in 256-byte chunks and waits for a 1-byte ACK.
  // Native USB CDC needs a larger queue because TinyUSB can deliver several
  // chunks before the cooperative transfer loop runs.
#if !defined(SIMULATOR) && FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE
  logSerial.setRxBufferSize(4096);
#else
  logSerial.setRxBufferSize(1024);
#endif
#if ARDUINO_USB_MODE
  logSerial.setTxBufferSize(1024);
#endif
  Serial.begin(115200);
#if !defined(SIMULATOR) && FREEINK_DEVICE_X4PRO && !ARDUINO_USB_MODE
  UsbSerialFileTransfer::registerUsbCdcOverflowHandler();
#endif
#if !defined(SIMULATOR) && LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif
  logSerialInit();

  HalSystem::begin();
#ifndef SIMULATOR
  // The network stack (lwIP's tiT task, the default event loop, Arduino's
  // event task) can never be torn down once started. Started by the first
  // Wi-Fi screen, those ~20 KB landed inside the largest internal block and
  // cut it from ~123 KB to 63-74 KB for the rest of the boot. Starting it
  // here, before the display and fonts allocate, keeps it out of the way.
  Network.begin();
#endif
  // checkPanic() clears the watchdog capture marker after a successful SD
  // dump, so retain the boot classification for the later activity route.
  const bool rebootedFromPanic = HalSystem::isRebootFromPanic();
  // Build identity first, so every log capture names the firmware it came from.
#ifdef SIMULATOR
  [[maybe_unused]] const char* runningPart = "sim";
#else
  const esp_partition_t* running = esp_ota_get_running_partition();
  [[maybe_unused]] const char* runningPart = running ? running->label : "?";
#endif
  LOG_INF("BOOT", "fw=%s sha=%s%s br=%s env=%s build=%s %s part=%s reset=%s", AppVersion::version(),
          BuildInfo::gitSha(), strcmp(BuildInfo::gitDirty(), "1") == 0 ? "*" : "", BuildInfo::gitBranch(),
          CROSSDINK_PIOENV, BuildInfo::buildNumber(), BuildInfo::buildTime(), runningPart,
          resetReasonName(rawResetReason));
  LOG_INF("BOOT", "Reset diagnostic: reset=%d(%s) sleepWake=%d(%s)", static_cast<int>(rawResetReason),
          resetReasonName(rawResetReason), static_cast<int>(rawWakeupCause), wakeupCauseName(rawWakeupCause));
  PerfLog::logLastSleep();
#ifndef SIMULATOR
  {
    char sec[96];
    DeviceSecurity::format(sec, sizeof(sec));
    LOG_INF("BOOT", "sec: %s", sec);
  }
#endif

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Validate the target too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const bool isValidSilentTarget =
      silentRebootTarget <= SILENT_REBOOT_TARGET_READER || isNetworkBootTargetValue(silentRebootTarget);
  const uint32_t snapshotTarget = (isSilentReboot && isValidSilentTarget) ? silentRebootTarget : 0;
  const uint32_t snapshotPayload = (isSilentReboot && isValidSilentTarget) ? silentRebootPayload : 0;
  const bool cleanImageBaseOnEntry =
      snapshotTarget == SILENT_REBOOT_TARGET_READER && (snapshotPayload & SILENT_REBOOT_READER_CLEAN_IMAGE_BASE) != 0;
  const bool isNetworkResume = snapshotTarget >= static_cast<uint32_t>(NetworkBootTarget::OTA);
  // Network screens need the reader-sized render stack on S3, including OTA
  // and KOReader Auth after their Wi-Fi child completes. C3 retains the smaller
  // network stack to preserve internal RAM.
  const bool useReaderRenderStack = !isNetworkResume || FREEINK_MCU_S3;
  const bool hasSilentRebootLight = isSilentReboot && (silentRebootFrontlight == SILENT_REBOOT_FRONTLIGHT_ON ||
                                                       silentRebootFrontlight == SILENT_REBOOT_FRONTLIGHT_OFF);
  const bool silentRebootLightOn = silentRebootFrontlight == SILENT_REBOOT_FRONTLIGHT_ON;
  silentRebootMagic = 0;
  silentRebootTarget = 0;
  silentRebootPayload = 0;
  silentRebootFrontlight = 0;
  if (!isSilentReboot || snapshotTarget != SILENT_REBOOT_TARGET_READER) {
    clearSilentRestartReaderPageBuild();
  }
  if (!isSilentReboot || snapshotTarget != SILENT_REBOOT_TARGET_HOME) {
    silentFirmwareUpdateMagic = 0;
  }

  gpio.begin();
  // Sticky shares Confirm and Power on one GPIO. Emit Power first so the
  // configured shortcut wins; MappedInputManager mirrors it back to Confirm
  // only on screens that explicitly allow the fallback.
  gpio.setSharedConfirmPowerShortPressEmitsPower(true);
  powerManager.begin();
  InputWake::begin();
  PinMon::begin();
  PerfLog::setWakeCounter(&InputWake::takeWakeCounts);
  PerfLog::setWakePinsDescriber(&InputWake::describePins);
  PerfLog::setPmWindowHook(&CoreLoadLog::logQuietWindowTasks);

  const auto wakeupReason = gpio.getWakeupReason();
#ifndef SIMULATOR
  // Marks for the gap between "Input wake armed" and the IMU/RTC lines.
  LOG_INF("BOOT", "mark: power-button wake check");
  const bool shortPressWakes = readWakeShortPressFromNvs();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton &&
      !gpio.verifyPowerButtonWakeup(shortPressWakes, CrossPointSettings::POWER_BUTTON_LONG_PRESS_MS)) {
    LOG_DBG("MAIN", "Power-button wake not held through verification, sleeping");
    PerfLog::noteDeepSleep("wake-not-held", "boot");
    armSleepGuard();
    BatteryLog::noteFalseWake();
    powerManager.startDeepSleep(gpio);
  }
#endif

#ifndef SIMULATOR
  // X4 Pro and X4 Classic both map Up to the GPIO0 boot strap. Use Down for
  // recovery so holding the recovery chord cannot strand either S3 board in a
  // boot-mode loop.
  const auto recoveryButton = (BoardConfig::isX4Pro() || CROSSDINK_APP_DEVICE_X4CLASSIC)
                                  ? MappedInputManager::Button::Down
                                  : MappedInputManager::Button::Up;
  const bool recoveryFirmwareMode = wakeupReason == HalGPIO::WakeupReason::PowerButton && !BoardConfig::isPaperMono() &&
                                    mappedInputManager.isPressed(recoveryButton);
#else
  const bool recoveryFirmwareMode = false;
#endif

  LOG_INF("BOOT", "mark: IMU probe");
  halTiltSensor.begin();
  LOG_INF("BOOT", "mark: RTC read");
  halClock.begin();
#ifndef SIMULATOR
  // Charger STAT wake (battery log builds): note the charge start/stop and sleep again.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    BatteryLog::onChargeWake();
    PerfLog::noteDeepSleep("charge-wake", "boot");
    armSleepGuard();
    powerManager.startDeepSleep(gpio);
  }
#endif

  // One-shot, consumed only once the wake is real (a press too short to wake and
  // a charger wake go back to sleep above with the flag still armed). Cleared
  // before any painting so a hang in the blocking paint path resets into a
  // normal splash boot instead of a splashless loop with no frame.
  const bool splashlessWakeArmed = splashlessWakeMagic == SPLASHLESS_WAKE_MAGIC;
  splashlessWakeMagic = 0;

#if FREEINK_DEVICE_X4 || FREEINK_DEVICE_X3
  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  LOG_INF("BOOT", "Post-GPIO diagnostic: device=%s usb=%d silentReboot=%d silentTarget=%lu",
          gpio.deviceIsX3() ? "X3" : "X4", gpio.isUsbConnected() ? 1 : 0, isSilentReboot ? 1 : 0,
          static_cast<unsigned long>(snapshotTarget));
#else
#ifdef SIMULATOR
  LOG_INF("MAIN", "Device: Simulator");
#else
  LOG_INF("MAIN", "Device: %s", BoardConfig::ACTIVE.name);
#endif
#endif

  LOG_INF("BOOT", "Wake route: %s (reset reason %d, wakeup cause %d)", wakeupRouteName(wakeupReason),
          static_cast<int>(esp_reset_reason()), static_cast<int>(esp_sleep_get_wakeup_cause()));
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      wakePowerReleasePending = true;
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // TEMP: continue booting while diagnosing post-flash/reset behavior.
      // Normal behavior is to go back to sleep when USB power causes a cold boot.
      LOG_INF("BOOT", "AfterUSBPower route: TEMP continuing boot instead of deep sleep");
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
      LOG_INF("BOOT", "AfterFlash route: continuing boot");
      break;
    case HalGPIO::WakeupReason::Other:
    default:
      LOG_INF("BOOT", "Other wake route: continuing boot");
      break;
  }

  bootPhase("start");
  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(isSilentReboot, !isNetworkResume, useReaderRenderStack);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    disarmBootGuard();
    return;
  }
  logBootHeap("storage ready");
  bootPhase("sd");

  HalSystem::checkPanic();
  // Before anything reads progress.bin: replay a position a crash or reset kept
  // off the card.
  ReaderProgressShadow::recoverPending();

  SETTINGS.loadFromFile();
#if CROSSDINK_GOODIES
  knobs::load(mappedInputManager.isPressed(MappedInputManager::Button::Back));
#endif
  Storage.installDateTimeCallback(LocalClock::offsetQAtUtc);
  APP_STATE.loadFromFile();
  mirrorWakeShortPressToNvs();
  // Needs SETTINGS for the clock's UTC offset, so it cannot run any earlier.
  BatteryDiagnosticLog::record(BatteryDiagnosticLog::Event::Wake, BoardConfig::ACTIVE.name,
                               wakeupRouteName(wakeupReason));
  BatteryLog::onBoot();
  const bool isSleepWake = wakeupReason == HalGPIO::WakeupReason::PowerButton;
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  // Normal boot store deferral adapted from Sichroteph/YACP commit
  // 20af8aee8d3e1d560456753b08d1f52e5488621f (MIT). Accessors load these
  // stores when Home, reader bookkeeping, or sync actually need them.
  if (!isNetworkResume) {
    Dictionary::isValidDictionary();
  } else if (snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::KOREADER_SYNC) ||
             snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::KOREADER_AUTH) ||
             snapshotTarget == static_cast<uint32_t>(NetworkBootTarget::FILE_TRANSFER)) {
    KOREADER_STORE.loadFromFile();
  }
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);
  logBootHeap("boot state ready");
  bootPhase("settings");
  // Silent restarts are invisible recovery steps, so they always retain the
  // current light state rather than applying wake or schedule policy. The
  // saved flag can be stale (e.g. schedule kept the light off this wake), so
  // prefer the live state the previous run handed over in RTC memory.
  const bool wasLightOnBeforeSleep =
      FrontlightSchedule::lightStateBeforeStart(hasSilentRebootLight, silentRebootLightOn, SETTINGS.frontlightOn != 0);
  const bool preserveLightAcrossRestart = FrontlightSchedule::shouldPreserveLightAcrossRestart(isSilentReboot);
  bool restoreLightOn = FrontlightSchedule::shouldRestoreLightOnStart(
      preserveLightAcrossRestart, SETTINGS.frontlightRestoreOnWake != 0, wasLightOnBeforeSleep);
  if (FrontlightSchedule::shouldApplyOnWakeSchedule(preserveLightAcrossRestart, SETTINGS.frontlightRestoreOnWake != 0,
                                                    wasLightOnBeforeSleep) &&
      FrontlightSchedule::hasCompleteWindow(SETTINGS.frontlightScheduleEnabled != 0, SETTINGS.frontlightScheduleStart,
                                            SETTINGS.frontlightScheduleEnd)) {
    uint8_t utcHour = 0;
    uint8_t utcMinute = 0;
    if (halClock.getTime(utcHour, utcMinute)) {
      const uint16_t localTimeOfDay =
          FrontlightSchedule::localTimeOfDay(utcHour, utcMinute, LocalClock::currentOffsetQ());
      restoreLightOn = FrontlightSchedule::containsTimeOfDay(SETTINGS.frontlightScheduleStart,
                                                             SETTINGS.frontlightScheduleEnd, localTimeOfDay);
    } else {
      restoreLightOn = false;
    }
  }
  Frontlight.releaseAfterWake();
  Frontlight.begin(SETTINGS.frontlightBrightness, SETTINGS.frontlightWarmth, restoreLightOn);
  BatteryLog::lightChanged();  // onBoot ran before the light was set up

  if (recoveryFirmwareMode) {
    LOG_INF("MAIN", "Recovery firmware mode (%s + POWER held at boot)",
            (BoardConfig::isX4Pro() || CROSSDINK_APP_DEVICE_X4CLASSIC) ? "DOWN" : "UP");
  }

  LOG_DBG("MAIN", "Starting CrossDink version %s", AppVersion::version());
  logMemoryStats("Boot");

  // Resolve the single boot-presentation decision. Skipping the splash also
  // skips the panel-clearing pass and the X3 initial-full-sync arming (see
  // HalDisplay::begin), so the first paint is FAST_REFRESH (~500ms) over the
  // retained frame and input dispatches against a visible UI.
  if (APP_STATE.quickLockResumePending) {
    // A timeout wake starts an unlocked session. Keep this separate from the
    // persisted short Power-button wake policy used before settings load.
    APP_STATE.quickLockResumePending = false;
    APP_STATE.quickLockRestoreFrontlight = false;
    APP_STATE.saveToFile();
  }
  // A boot-screen folder or an explicitly selected BMP opts a reader into
  // seeing its boot image after a power-button wake as well as a cold boot.
  // Without either, retain the fast splashless resume path.
  bool hasBootScreenDirectory = false;
  bool hasPinnedBootScreen = false;
  if (SETTINGS.customBootscreenEnabled && isSleepWake && splashlessWakeArmed) {
    std::string bootScreenDirectory;
    hasBootScreenDirectory = ImageFolderIndex::resolveBootScreenDirectory(bootScreenDirectory);
    hasPinnedBootScreen = !APP_STATE.favoriteBootImagePath.empty() &&
                          FsHelpers::hasBmpExtension(APP_STATE.favoriteBootImagePath) &&
                          Storage.exists(APP_STATE.favoriteBootImagePath.c_str());
  }
  const bool skipSplashOnWake = isSleepWake && splashlessWakeArmed && !hasBootScreenDirectory && !hasPinnedBootScreen;
  const BootResume resume = isNetworkResume    ? BootResume::Network
                            : isSilentReboot   ? BootResume::Silent
                            : skipSplashOnWake ? BootResume::SplashlessWake
                                               : BootResume::Splash;
  bool isUc8279X3 = false;
#ifndef SIMULATOR
  isUc8279X3 = gpio.deviceIsX3() && BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX3Uc8279;
#endif
  const bool hasValidSleepFrame = resume == BootResume::SplashlessWake && isUc8279X3 && preflightSleepFrameBuffer();
  const bool shouldRestoreSleepFrame =
      resume == BootResume::SplashlessWake && (isUc8279X3 ? hasValidSleepFrame : Storage.exists(SLEEP_FRAME_FILE));
  bool allowFastInitialReaderRefresh = false;
  bool x4WakeFrameAlreadyCleaned = false;

  // A plain software restart that left its frame behind (restartKeepingPanelFrame,
  // e.g. after an SD firmware update) also starts seamlessly: the splash is
  // then a Fast refresh from the known panel content instead of a full flash.
  const bool retainedFrameBoot =
      resume == BootResume::Splash && rawResetReason == ESP_RST_SW && retainedPanelFramePresent();
  setupDisplayAndFonts(
      SleepWakePolicy::shouldInitializeSeamlessly(resume, isUc8279X3, hasValidSleepFrame) || retainedFrameBoot,
      resume != BootResume::Network, useReaderRenderStack);
  logBootHeap("display and font resolver ready");
  bootPhase("display");

  switch (resume) {
    case BootResume::Silent:
      // Splash skipped: the routing block below picks the target activity; the
      // panel keeps showing the pre-reboot popup until that first paint lands.
      break;
    case BootResume::Network:
      LOG_INF("BOOT", "Minimal network boot ready: target=%lu free=%" PRIu32 " maxAlloc=%" PRIu32,
              static_cast<unsigned long>(snapshotTarget), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      break;
    case BootResume::SplashlessWake:
      if (shouldRestoreSleepFrame && loadSleepFrameBuffer()) {
        if (gpio.deviceIsX3()) {
          // begin() clears the X3 controller RAM. Restore the saved frame as
          // the baseline for the first reader paint without refreshing the panel.
          renderer.cleanupGrayscaleWithFrameBuffer();
          allowFastInitialReaderRefresh = true;
        }
      } else if (isUc8279X3 && hasValidSleepFrame) {
        // The frame passed the size preflight but could not be read after display
        // setup. Do one clean, device-specific recovery rather than painting
        // differentially against an invalid controller baseline.
        LOG_ERR("BOOT", "Quick Resume frame load failed; rebuilding UC8279 X3 display baseline");
        Storage.remove(SLEEP_FRAME_FILE);
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      } else if (shouldClearX4WakeGhosting() && SETTINGS.fadingFix != 0) {
        LOG_INF("BOOT", "X4 wake: clearing retained sleep image with half refresh");
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        // The explicit HALF refresh above has already established a clean panel
        // baseline, so the reader's first page can use its fast initial cycle
        // instead of repeating the cleanup waveform.
        allowFastInitialReaderRefresh = true;
        x4WakeFrameAlreadyCleaned = true;
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot();
      break;
  }

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
  } else if (rebootedFromPanic) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
  } else if (resume == BootResume::Network) {
    const bool launched =
        launchNetworkTarget(static_cast<NetworkBootTarget>(snapshotTarget), snapshotPayload, /*inPlace=*/false);
    if (!launched) {
      LOG_ERR("MAIN", "Minimal network boot target failed; returning home");
      silentRestart();
    }
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath, false, false, cleanImageBaseOnEntry);
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    // X4's HALF refresh is the same single-pass clean transition already used
    // by network screens. Keep X3's existing full refresh behavior unchanged.
    // A seeded retained frame is the panel's true OLD plane, so Home is a plain
    // Fast transition from the exited screen (no flash).
    const auto homeRefreshMode = gpio.deviceIsX3()          ? HalDisplay::FULL_REFRESH
                                 : retainedPanelFrameSeeded ? HalDisplay::FAST_REFRESH
                                                            : HalDisplay::HALF_REFRESH;
    // File Transfer exit with a firmware to flash (POST /api/exit?flash=...):
    // open the update flow directly instead of over a live Home, whose
    // background Library walk would otherwise keep competing for the SD card
    // while the image is validated and flashed. Finishing pops an empty
    // stack, which lands on Home.
    const std::string pendingFirmware =
        snapshotTarget == SILENT_REBOOT_TARGET_HOME ? consumeSilentRestartFirmwareUpdate() : std::string();
    auto firmwareUpdate = pendingFirmware.empty() ? nullptr
                                                  : makeUniqueNoThrow<SdFirmwareUpdateActivity>(
                                                        renderer, mappedInputManager, false, pendingFirmware);
    if (firmwareUpdate) {
      LOG_INF("MAIN", "Opening firmware update for %s", pendingFirmware.c_str());
      // Clear the retained File Transfer frame the way Home's first paint would.
      {
        RenderLock lock;
        renderer.clearScreen();
        renderer.displayBuffer(homeRefreshMode);
      }
      activityManager.replaceActivity(std::move(firmwareUpdate));
    } else {
      if (!pendingFirmware.empty()) LOG_ERR("MAIN", "Cannot allocate firmware update for %s", pendingFirmware.c_str());
      activityManager.goHome(HomeMenuItem::NONE, homeRefreshMode);
    }
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) ||
             APP_STATE.readerActivityLoadCount() > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    // On X4, use the first Home paint to clean the retained sleep image.
    const auto homeRefreshMode =
        resume == BootResume::SplashlessWake && shouldClearX4WakeGhosting() && !x4WakeFrameAlreadyCleaned
            ? HalDisplay::HALF_REFRESH
            : HalDisplay::FAST_REFRESH;
    activityManager.goHome(HomeMenuItem::NONE, homeRefreshMode);
  } else {
    // Count the attempt in RTC so a book that crashes on load boots to Home next time.
    APP_STATE.setReaderActivityLoadCount(APP_STATE.readerActivityLoadCount() + 1);
    if (isSleepWake) kosync_auto::noteWake();
    activityManager.goToReader(APP_STATE.openEpubPath, false, allowFastInitialReaderRefresh);
  }

  bootPhase("route");
  if (resume == BootResume::Silent || resume == BootResume::Network) {
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // From here keys and touch are sampled on their own task, so events made
  // during long loop work are queued instead of dropped.
  InputTask::begin();

  allowSleepAt = millis() + BOOT_SLEEP_GRACE_MS;
  disarmBootGuard();
}

namespace {
KNOB_ALIAS(IDLE_WAIT_MS, idleWaitMs);  // Goodies > Knobs, as the two below
KNOB_ALIAS(IDLE_WAIT_SETTLED_MS, idleWaitSettledMs);
KNOB_ALIAS(IDLE_WAIT_LONG_MS, idleWaitLongMs);
// Toasts, hold thresholds and the Home double tap all resolve within a couple
// of seconds of the last input, so the idle tick stays short until then.
KNOB_ALIAS(IDLE_WAIT_BACKOFF_AFTER_MS, idleBackoffAfterMs);

bool anyInputHeld() {
  for (uint8_t button = HalGPIO::BTN_BACK; button <= HalGPIO::BTN_POWER; ++button) {
    if (gpio.isPressed(button)) return true;
  }
#if CROSSDINK_APP_CAP_TOUCH
  float nx = 0.0f;
  float ny = 0.0f;
  if (gpio.isTouchHeldAt(nx, ny)) return true;
#endif
  return false;
}

// The current screen opted into radio idle, or the radio only carries the
// Goodies Wi-Fi remote's idle server.
static bool radioMayIdle() {
#if CROSSDINK_GOODIES
  if (goodies_remote::allowsRadioIdleSleep()) return true;
#endif
  return activityManager.allowsRadioIdleSleep();
}

// Longest idle wait for the power-saving branch of loop(). When every input
// is on an InputWake line the tick only paces timers, so it backs off the
// longer the device sits untouched. Anything still polled keeps 50 ms.
// Flash duck: fade the frontlight out from when the driver plans a flashing
// refresh (HalDisplay::flashPlannedMs: on UC8179 at refresh entry, before its
// power and SPI work) so it reaches the Flash Dim Level as the swing shows
// (HalDisplay::flashStartedMs: on UC8179 the driver's DRF time plus the
// waveform's own offset: direct gray drives the background black from frame
// 24 of 50), and back up over FLASH_DUCK_UP_MS once the refresh ends. Until DRF
// the swing time is the plan time plus the last measured plan-to-dark time of
// that kind, then the fade retargets from where it is. An unplanned flash
// fades over at least FLASH_DUCK_DOWN_MS. Goodies > Knobs flash*DimMs /
// flash*RestoreMs shift both ends, per waveform (HalDisplay::flashKind: AA
// page, Half/Full, DU paint). Runs alongside the refresh (render task) and
// never waits on it. A mark older than FLASH_DUCK_MAX_MS is an async refresh
// nobody waited on, so it ends. The times are Goodies > Knobs; a zero fade
// never divides (both ramps test it first).
KNOB_ALIAS(FLASH_DUCK_DOWN_MS, flashDownMs);
KNOB_ALIAS(FLASH_DUCK_MAX_MS, flashMaxMs);
KNOB_ALIAS(FLASH_DUCK_UP_MS, flashUpMs);
KNOB_ALIAS(FLASH_DUCK_TICK_MS, flashTickMs);
static bool flashDuckActive = false;
static uint8_t flashDuckLevel = 100;
static unsigned long flashDuckUpStartMs = 0;
static unsigned long flashDuckInputMs = 0;  // last user input (the fade starts there)

static uint32_t liveFlashStartMs() {
  const uint32_t startMs = display.flashStartedMs();
  // Signed: the swing can lie ahead, and is live until then.
  return startMs != 0 && static_cast<int32_t>(millis() - startMs) <= static_cast<int32_t>(FLASH_DUCK_MAX_MS) ? startMs
                                                                                                             : 0;
}

static void updateFlashDuck() {
  const unsigned long now = millis();
  const unsigned long inputMs = flashDuckInputMs;
  const uint32_t swingMs = liveFlashStartMs();
  const uint32_t markMs = display.flashPlannedMs();
  static uint32_t swungMarkMs = 0;  // the mark whose swing showed (its mark no longer ducks)
  const bool marked = markMs != 0 && markMs != swungMarkMs && now - markMs <= FLASH_DUCK_MAX_MS;
  static uint32_t swingEndMs = 0;                  // expected end of the swing being tracked
  static uint32_t swingGoneMs = 0;                 // when it ended (for a later restore)
  static auto kind = HalDisplay::FlashKind::Full;  // of that swing, kept for a late restore
  if (swingMs != 0) {
    swingEndMs = display.flashEndsMs();
    swingGoneMs = 0;
    kind = display.flashKind();
    swungMarkMs = markMs;
  } else {
    if (swingEndMs != 0) {
      swingEndMs = 0;
      swingGoneMs = now | 1;
    }
    if (marked) kind = display.flashPlannedKind();
  }
  // Goodies offsets (later is positive). The refresh never waits on either: an
  // earlier dim than the driver can announce just cuts the light at DRF.
  const bool gray = kind == HalDisplay::FlashKind::Gray, full = kind == HalDisplay::FlashKind::Full;
  const bool grayDark = kind == HalDisplay::FlashKind::GrayDark;
  const int32_t dimMs = gray       ? KNOBS.flashGrayDimMs
                        : grayDark ? KNOBS.flashGrayDarkDimMs
                        : full     ? KNOBS.flashFullDimMs
                                   : KNOBS.flashPaintDimMs;
  const int32_t restoreMs = gray       ? KNOBS.flashGrayRestoreMs
                            : grayDark ? KNOBS.flashGrayDarkRestoreMs
                            : full     ? KNOBS.flashFullRestoreMs
                                       : KNOBS.flashPaintRestoreMs;
  // Up early: before the expected end (negative restore). Up late: hold dark
  // after the refresh ended (positive restore).
  const bool restoreEarly =
      swingMs != 0 && restoreMs < 0 && swingEndMs != 0 && static_cast<int32_t>(now - (swingEndMs + restoreMs)) >= 0;
  const bool holdLate = swingMs == 0 && swingGoneMs != 0 && static_cast<int32_t>(now - (swingGoneMs + restoreMs)) < 0;
  const bool ducking = ((swingMs != 0 || marked) && !restoreEarly) || (flashDuckActive && holdLate);
  // Plan to dark per kind (Gray, Full, Paint), re-measured by every planned
  // flash: the driver's own SPI/power work, so it barely varies (render time
  // and queueing come before the plan; input to dark ran 109-448 ms). Seeds:
  // logs/device/20261001T040341Z-32cc8fdd-ptests.txt L6211->L6215 74 ms and
  // L6236->L6240 69 ms (gray LSB to DRF; the default Gray dim is dark at DRF);
  // Full 182 ms (boot Home paint: PON 127 + SPI 27, 20261002T011313Z-c0142b63
  // L46-50; RAM only, so every boot starts from it); Paint (POF 82 + PON 127 +
  // SPI 54 + reset) inferred.
  constexpr uint32_t kMaxLeadMs = 600;
  static uint16_t leadMs[4] = {75, 180, 330, 75};  // by FlashKind (GrayDark plans like Gray)
  static uint8_t leadKind = 0;
  static bool learned = false;      // this flash's lead is stored
  static uint32_t fadeStartMs = 0;  // the plan (or the start) the fade counts from
  static uint32_t planMs = 0;       // when the driver planned this flash (0: unplanned)
  static uint32_t fromMs = 0, darkMs = 0;
  static uint8_t fromLevel = 100;  // the fade runs from (fromMs, fromLevel) to the floor at darkMs
  // The user (brightness, toggle, Quick Lock) or the light timeout took over.
  if (flashDuckActive && Frontlight.idleDimPercent() != flashDuckLevel) flashDuckActive = false;
  const bool fresh = !flashDuckActive || flashDuckUpStartMs != 0;  // a new flash (back-to-back: from here)
  // Never duck below 2% of full: lower duty shifts the frontlight color. Every duck level below derives from this.
  const unsigned long minPct = std::max<unsigned long>(KNOBS.flashDuckMinPct, 2);
  if (!flashDuckActive) {
    if (!ducking || holdLate || !SETTINGS.frontlightFlashDuck || !Frontlight.isOn() ||
        Frontlight.idleDimPercent() != 100) {
      return;
    }
    if (Frontlight.brightness() <= minPct) {
      static uint32_t skippedMs = 0;  // logged once per flash
      const uint32_t flashMs = markMs != 0 ? markMs : swingMs;
      if (flashMs != skippedMs) {
        skippedMs = flashMs;
        LOG_DBG("LIGHT", "Flash duck: skipped (below floor)");
      }
      return;
    }
    flashDuckActive = true;
    flashDuckLevel = 100;
    flashDuckUpStartMs = 0;
  }
  // Each ramp moves only one way from where the light is, between 100% and
  // the Flash Dim Level (a % of the user's brightness; 0 = dark), but never
  // below minPct (>= 2) of full (rounded up; at 100 the duck is over).
  const unsigned long b = std::max<unsigned long>(Frontlight.brightness(), 1);
  const unsigned long floor = std::min<unsigned long>(
      std::max<unsigned long>(std::min<unsigned long>(SETTINGS.flashDuckDepth, 90), (minPct * 100 + b - 1) / b), 100);
  static bool darkLogged = false;
  unsigned long level;
  if (ducking) {
    if (fresh && !holdLate) {
      // From the plan, the earliest sign of a flash (input to plan is render
      // time, ~150 ms: a fade from the input stepped down ~60% at the plan,
      // logs/device/20261001T041615Z-32cc8fdd L6350->L6357).
      planMs = marked ? markMs : 0;
      fadeStartMs = planMs != 0 ? planMs : now;
      fromMs = fadeStartMs;
      fromLevel = flashDuckLevel;
      darkMs = 0;
      darkLogged = false;
      learned = false;
      leadKind = static_cast<uint8_t>(kind);
    }
    uint32_t target = swingMs != 0 ? swingMs + dimMs : planMs + leadMs[leadKind];
    if (swingMs != 0 && planMs != 0 && !learned) {
      learned = true;
      const int32_t lead = static_cast<int32_t>(target - planMs);
      leadMs[leadKind] = static_cast<uint16_t>(std::clamp<int32_t>(lead, 0, kMaxLeadMs));
      LOG_DBG("LIGHT", "Flash duck: plan to dark %ld ms, input to dark %ld ms (kind %u)", static_cast<long>(lead),
              static_cast<long>(target - inputMs), leadKind);
    }
    if (planMs == 0 && static_cast<int32_t>(target - (fadeStartMs + FLASH_DUCK_DOWN_MS)) < 0) {
      target = fadeStartMs + FLASH_DUCK_DOWN_MS;
    }
    if (darkMs == 0) {
      LOG_DBG("LIGHT", "Flash duck: down, dark in %ld ms", static_cast<long>(target - now));
    } else if (target != darkMs) {
      fromMs = now;  // the driver's swing time: retarget from where the fade is
      fromLevel = flashDuckLevel;
    }
    darkMs = target;
    const int32_t left = static_cast<int32_t>(target - now);
    const unsigned long from = std::max<unsigned long>(fromLevel, floor);
    level = std::max(floor, std::min<unsigned long>(
                                flashDuckLevel, left <= 0 ? floor : floor + (from - floor) * left / (target - fromMs)));
    if (level == floor && !darkLogged) {
      darkLogged = true;
      if (swingMs != 0) {
        LOG_DBG("LIGHT", "Flash duck: dark at %lu (DRF%+ld)", now, static_cast<long>(now - swingMs));
      } else {
        LOG_DBG("LIGHT", "Flash duck: dark at %lu (DRF pending)", now);
      }
    }
    flashDuckUpStartMs = 0;  // a back-to-back flash keeps it down
  } else {
    if (flashDuckUpStartMs == 0) {
      flashDuckUpStartMs = now;
      LOG_DBG("LIGHT", "Flash duck: up at %u%%", flashDuckLevel);
    }
    const unsigned long elapsed = now - flashDuckUpStartMs;
    level = std::max<unsigned long>(
        flashDuckLevel, elapsed >= FLASH_DUCK_UP_MS ? 100 : floor + (100 - floor) * elapsed / FLASH_DUCK_UP_MS);
  }
  if (level != flashDuckLevel) {
    if (level == 100) {
      const uint32_t endMs = swingMs != 0 ? swingEndMs : swingGoneMs;
      LOG_DBG("LIGHT", "Flash duck: restored at %lu (end%+ld), duty %u%%", now, static_cast<long>(now - endMs),
              Frontlight.brightness());
    }
    flashDuckLevel = static_cast<uint8_t>(level);
    Frontlight.setIdleDim(flashDuckLevel);
  }
  if (!ducking && flashDuckLevel == 100) flashDuckActive = false;
}

// The main loop can block in a render wait through a whole refresh (a reader
// redraw under the drawer: log 20261001T021833Z-2c6751af L3220-3227, loop 1377
// ms), so RenderLock's waits step the duck too, on this task only.
static TaskHandle_t mainLoopTask = nullptr;
static void flashDuckRenderWait() {
  if (xTaskGetCurrentTaskHandle() != mainLoopTask) return;
  TransferLightPulse::updateBlink();  // a remote blink ends on time, not after the refresh
  updateFlashDuck();
}
void installFlashDuckRenderWait() {
  mainLoopTask = xTaskGetCurrentTaskHandle();  // setup() and loop() share the Arduino loop task
  RenderLock::waitTick = &flashDuckRenderWait;
}

// A running transfer pulse owns the light (the dim doesn't show under it), so the
// Light Timeout counts from the pulse's last step: fading during it would
// flicker, and a pulse longer than the timeout would end in a snap to dark.
static unsigned long lastPulseMs = 0;
static unsigned long lightIdleMs(const unsigned long idleMs) {
  if (TransferLightPulse::animating()) lastPulseMs = millis();
  return std::min(idleMs, millis() - lastPulseMs);
}

uint32_t idleWaitMs(const unsigned long idleMs) {
  if (TransferLightPulse::animating()) return TransferLightPulse::WRITE_INTERVAL_MS;
  if (SerialRemote::macroRunning()) return IDLE_WAIT_MS;  // macro WAIT/WAITLOG steps resolve on the loop tick
  if (flashDuckActive || ((liveFlashStartMs() != 0 || display.flashMarkedMs() != 0 || display.flashPlannedMs() != 0) &&
                          SETTINGS.frontlightFlashDuck)) {
    return FLASH_DUCK_TICK_MS;
  }
  if (!InputWake::coversAllInputs() || idleMs < IDLE_WAIT_BACKOFF_AFTER_MS) return IDLE_WAIT_MS;
  // Light timeout: wake on time for the fade and step it at the fast tick.
  const unsigned long lightTimeoutMs = SETTINGS.getFrontlightTimeoutMs();
  if (lightTimeoutMs > 0 && Frontlight.isOn() && Frontlight.idleDimPercent() > 0 &&
      lightIdleMs(idleMs) + IDLE_WAIT_LONG_MS >= lightTimeoutMs) {
    return IDLE_WAIT_MS;
  }
  const bool tiltPolling = SETTINGS.tiltPageTurn != CrossPointSettings::TILT_OFF && halTiltSensor.isAvailable() &&
                           activityManager.isReaderActivity();
#ifdef SIMULATOR
  const bool usbConnected = gpio.isUsbConnected();
#else
  const bool usbConnected = gpio.isUsbConnectedCached();
#endif
  // Timed activity work (automatic page turn), USB serial transfer and radio
  // exchanges are paced by the tick rather than by input.
  const bool radioIdle = radioMayIdle();
  if (tiltPolling || usbConnected || anyInputHeld() ||
      (!radioIdle && (activityManager.preventAutoSleep() || WiFi.getMode() != WIFI_MODE_NULL))) {
    return IDLE_WAIT_MS;
  }
  // An idle server on its own task (File Transfer, Calibre) or a screen that opted
  // into radio idle (OPDS list, KOSync result) only needs the loop for input,
  // exit requests and link checks: 4 wakes/s instead of 20.
#if CROSSDINK_GOODIES
  // Only the Wi-Fi remote holds the radio: its server wakes on traffic and
  // /api/cmd wakes the loop (InputTask::wakeLoop), so the long tick.
  if (goodies_remote::allowsRadioIdleSleep()) return IDLE_WAIT_LONG_MS;
#endif
  if (radioIdle) return IDLE_WAIT_SETTLED_MS;
  return IDLE_WAIT_LONG_MS;
}

#if CROSSDINK_APP_CAP_TOUCH && !defined(SIMULATOR)
// Quick Lock triggers that only the physical keys can lift. The Home-key
// triggers and the Back/Menu holds read the touch controller, so it stays
// awake for those.
bool quickLockUnlocksWithKeys(const QuickLockTrigger trigger) {
  switch (trigger) {
    case QuickLockTrigger::ShortPower:
    case QuickLockTrigger::LongPower:
    case QuickLockTrigger::PowerUp:
    case QuickLockTrigger::UpDown:
      return true;
    default:
      return false;
  }
}

// Sleeps the GT911 while nothing reads it: under a Quick Lock only the keys
// can lift, or on a reader page with the touchscreen disabled and the Home key
// locked (the Home key is part of the GT911). The keys stay on InputWake, and
// any change that needs touch again wakes it on the next loop.
void updateTouchControllerSleep() {
  static unsigned long retryAt = 0;
  static bool retryPending = false;
  if (!gpio.hasTouch()) return;
  const bool quickLockedByKeys =
      buttonShortcutController.isQuickLocked() && quickLockUnlocksWithKeys(buttonShortcutController.quickLockTrigger());
  const bool readerTouchOff = activityManager.isReaderActivity() && !mappedInputManager.hasTouch() &&
                              (!mappedInputManager.hasHomeKey() || mappedInputManager.isHomeButtonLockedInReader());
  const bool wantAsleep = quickLockedByKeys || readerTouchOff;
  if (wantAsleep == gpio.isTouchAsleep()) {
    retryPending = false;
    return;
  }
  if (retryPending && static_cast<long>(millis() - retryAt) < 0) return;
  if (gpio.setTouchSleep(wantAsleep)) {
    retryPending = false;
    LOG_DBG("TOUCH", "Touch controller %s", wantAsleep ? "asleep" : "awake");
  } else {
    // Refused while a finger or the Home key is down, or no answer on I2C.
    retryPending = true;
    retryAt = millis() + 1000;
  }
}
#endif
}  // namespace

#if CROSSDINK_PERF_LOG
// Debug trace of discrete input events ([IN]): button edges, taps, long
// presses, swipes, the home key and tilt turns. Drag samples are left out.
// Touch coordinates are panel-normalized per mille (0-1000). Returns the kind
// of this frame's event for the [LAT] line.
static const char* logInputEvents(uint32_t& seq) {
  static constexpr const char* BUTTON_NAMES[] = {"back", "confirm", "left", "right", "up", "down", "power"};
  // #N is taken by the first [IN] line of the frame; contact moves alone stay #0.
  seq = 0;
  const auto id = [&seq] {
    if (seq == 0) seq = PerfLog::nextInputSeq();
    return static_cast<unsigned long>(seq);
  };
  const char* kind = "touch";  // contact moves only
  for (uint8_t i = 0; i < sizeof(BUTTON_NAMES) / sizeof(BUTTON_NAMES[0]); i++) {
    if (gpio.wasPressed(i)) {
      LOG_DBG("IN", "#%lu btn %s down", id(), BUTTON_NAMES[i]);
      kind = "btn";
    }
    if (gpio.wasReleased(i)) {
      LOG_DBG("IN", "#%lu btn %s up", id(), BUTTON_NAMES[i]);
      kind = "btn";
    }
  }
#if CROSSDINK_APP_CAP_TOUCH
  const auto permille = [](const float n) { return static_cast<int>(n * 1000.0f); };
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if (gpio.wasHomeKeyTapped()) {
    LOG_DBG("IN", "#%lu home key", id());
    kind = "home";
  }
  // Screen px in the current orientation (what the UI acts on), then the raw
  // panel permille for touch debugging. The panel is rotated from the held
  // orientation, so directions come from the logical points.
  int lx0 = 0, ly0 = 0, lx1 = 0, ly1 = 0;
  if (gpio.wasTouchTap(x0, y0)) {
    renderer.tapToLogical(x0, y0, lx0, ly0);
    LOG_DBG("IN", "#%lu tap %d,%d (panel %d,%d)", id(), lx0, ly0, permille(x0), permille(y0));
    kind = "tap";
  } else if (gpio.wasTouchLongPress(x0, y0)) {
    renderer.tapToLogical(x0, y0, lx0, ly0);
    LOG_DBG("IN", "#%lu long %d,%d (panel %d,%d)", id(), lx0, ly0, permille(x0), permille(y0));
    kind = "long";
  } else if (gpio.wasSwipe(x0, y0, x1, y1)) {
    renderer.tapToLogical(x0, y0, lx0, ly0);
    renderer.tapToLogical(x1, y1, lx1, ly1);
    const int dx = lx1 - lx0;
    const int dy = ly1 - ly0;
    const char* dir = std::abs(dx) >= std::abs(dy) ? (dx < 0 ? "left" : "right") : (dy < 0 ? "up" : "down");
    LOG_DBG("IN", "#%lu swipe %s %d,%d->%d,%d (panel %d,%d->%d,%d)", id(), dir, lx0, ly0, lx1, ly1, permille(x0),
            permille(y0), permille(x1), permille(y1));
    kind = "swipe";
  }
#endif
  // The loop only calls this on input, and hadActivity() consumes its flag, so
  // input with no button or touch event is a tilt turn.
  bool touchActivity = false;
#if CROSSDINK_APP_CAP_TOUCH
  touchActivity = gpio.wasTouchActivity();
#endif
  if (strcmp(kind, "touch") == 0 && !touchActivity) {
    LOG_DBG("IN", "#%lu tilt", id());
    kind = "tilt";
  }
  return kind;
}

#if CROSSDINK_APP_CAP_TOUCH
// [IN] touch noise: touch INT wakes that formed no gesture (TouchNoiseMonitor).
// Every pass, since noise that forms no contact never wakes the loop. Log only.
static void logTouchNoise(const char* inputKind) {
  static TouchNoiseMonitor monitor;
  HalGPIO::CompletedMultiTouchSwipe swipe{};
  HalGPIO::CompletedMultiTouchRotation rotation{};
  const bool gesture = (inputKind != nullptr && strcmp(inputKind, "touch") != 0 && strcmp(inputKind, "btn") != 0 &&
                        strcmp(inputKind, "tilt") != 0) ||
                       gpio.wasCompletedMultiTouchSwipe(swipe) || gpio.wasCompletedMultiTouchRotation(rotation);
  float nx = 0, ny = 0;
  const bool contact = inputKind != nullptr && gpio.wasTouchDown(nx, ny);
  if (monitor.update(millis(), InputWake::touchWakeTotal(), gesture, contact)) {
    LOG_INF("IN", "touch noise: %lu wakes, 0 gestures, %lu contacts in %lus", static_cast<unsigned long>(monitor.wakes),
            static_cast<unsigned long>(monitor.contacts), static_cast<unsigned long>(monitor.spanMs / 1000));
  }
}
#endif
#endif

// Set by every wait at the end of a pass; early returns skip those waits.
static bool loopPassBlocked = false;

static void loopPass() {
  static unsigned long maxLoopDuration = 0;
  static unsigned long lastSlowLoopLog = 0;
  const unsigned long loopStartTime = millis();
  PerfLog::noteLoopPass();
  static unsigned long lastSysLog = 0;

  // Keep release suppression in the mapped-input layer in sync with every
  // hardware input frame. A shortcut may open an activity that never queries
  // the originating button, so its one-shot release guard must still expire.
  mappedInputManager.update();
#ifdef SIMULATOR
  simulatorHomeKeyInput.update();
#endif
  if (activityManager.requiresExclusiveStorageLoop()) {
    // Keep the serial endpoint responsive so Inky receives ERR:not_on_home,
    // while every filesystem/UI/global path remains suspended by the activity.
    (void)UsbSerialFileTransfer::process(false);
    activityManager.loop();
    if (activityManager.preventAutoSleep()) {
      powerManager.setPowerSaving(false);
      loopPassBlocked = true;
      delay(10);
    } else {
      // No host is active, so a slower loop is safe. The activity itself times
      // out the raw-storage handoff rather than entering deep sleep detached.
      powerManager.setPowerSaving(true);
      loopPassBlocked = true;
      delay(50);
    }
    return;
  }

#if CROSSDINK_APP_CAP_TOUCH && !defined(SIMULATOR)
  updateTouchControllerSleep();
#endif

  if (!buttonShortcutController.isQuickLocked()) {
    halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.tiltPageTurnDirection, SETTINGS.orientation,
                         activityManager.isReaderActivity());
  }

  renderer.setFadingFix(SETTINGS.fadingFix);

  // Not gated on Serial: without a USB host these lines still reach the
  // PSRAM log ring (debug builds), which is how they get read off the device.
  if (millis() - lastSysLog >= 2000) {
    logSystemLine();
    lastSysLog = millis();
  }
  Frontlight.flushLog();

  if (!buttonShortcutController.isQuickLocked() && UsbSerialFileTransfer::process(activityManager.isHomeActivity()) ==
                                                       UsbSerialFileTransfer::ProcessResult::ScreenshotRequested) {
    // The render task draws on the other core; hold the lock so the dump is
    // never a half-drawn frame.
    RenderLock lock;
    const uint32_t bufferSize = display.getBufferSize();
    logSerial.printf("SCREENSHOT_START:%" PRIu32 "\n", bufferSize);
    uint8_t* buf = display.getFrameBuffer();
    logSerial.write(buf, bufferSize);
    logSerial.printf("SCREENSHOT_END\n");
  }

  // Notify the active activity before global shortcut and gesture routes consume
  // the input and skip its loop() for this frame.
  const bool userInputReceived = gpio.wasAnyPressed() || gpio.wasAnyReleased()
#if CROSSDINK_APP_CAP_TOUCH
                                 || gpio.wasTouchActivity()
#endif
                                 || halTiltSensor.hadActivity();
#if CROSSDINK_PERF_LOG
  const char* inputKind = nullptr;
  if (userInputReceived) {
    uint32_t inputSeq = 0;
    inputKind = logInputEvents(inputSeq);
    PerfLog::noteInput(/*release=*/!gpio.wasAnyPressed() && gpio.wasAnyReleased(), inputKind, inputSeq);
  }
#if CROSSDINK_APP_CAP_TOUCH
  logTouchNoise(inputKind);
#endif
#endif

  // User input paces power saving. Background work that only has to keep the
  // device out of deep sleep (automatic page turn, sync screens) holds off the
  // sleep timeout separately, so it no longer pins the CPU at full clock.
  static unsigned long lastActivityTime = millis();
  static unsigned long lastSleepBlockTime = millis();
  if (userInputReceived) {
    activityManager.wakePanelEarly();  // PON while the finger is still down
    lastActivityTime = millis();       // Reset inactivity timer
    flashDuckInputMs = lastActivityTime;
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }
  if (activityManager.preventAutoSleep()) {
    lastSleepBlockTime = millis();
  }
  if (userInputReceived) {
    activityManager.notifyUserInput();
  }

  // Light timeout. Any input brings the light back and still acts, so the
  // first tap after a long idle is never lost.
  static bool lightTimedOut = false;  // the timeout dimmed it (not the flash duck)
  if (lightTimedOut && Frontlight.idleDimPercent() == 100) lightTimedOut = false;  // restored elsewhere
  if (userInputReceived && lightTimedOut) {
    lightTimedOut = false;
    Frontlight.setIdleDim(100);
    BatteryLog::lightChanged();
    LOG_DBG("LIGHT", "Light timeout: restored by input");
  }
  const unsigned long LIGHT_FADE_MS = KNOBS.lightFadeMs;  // Goodies > Knobs
  const unsigned long lightTimeoutMs = SETTINGS.getFrontlightTimeoutMs();
  if (lightTimeoutMs > 0 && Frontlight.isOn() && Frontlight.idleDimPercent() > 0) {
    const unsigned long idleMs = lightIdleMs(std::min(millis() - lastActivityTime, millis() - lastSleepBlockTime));
    if (idleMs >= lightTimeoutMs) {
      const unsigned long fadeMs = idleMs - lightTimeoutMs;
      const uint8_t level = fadeMs >= LIGHT_FADE_MS ? 0 : static_cast<uint8_t>(100 - fadeMs * 100 / LIGHT_FADE_MS);
      if (level < Frontlight.idleDimPercent()) {
        if (Frontlight.idleDimPercent() == 100) LOG_DBG("LIGHT", "Light timeout: fading after %lu ms", idleMs);
        Frontlight.setIdleDim(level);
        lightTimedOut = true;
        if (level == 0) BatteryLog::lightChanged(true);
      }
    }
  }
  updateFlashDuck();
  BatteryLog::poll(millis() - lastActivityTime);

  // Let wake continue as soon as its hold has been verified. The release can
  // arrive after setup, so consume that one input frame rather than making it
  // a page turn, refresh, or other short power-button action.
  if (wakePowerReleasePending && !gpio.isPressed(HalGPIO::BTN_POWER)) {
    wakePowerReleasePending = false;
    return;
  }

  const bool modalOwnsInput = activityManager.blocksGlobalInput();

  // Keep Power + Down screenshot-only. The configurable chord below uses Up,
  // so it cannot replace or double-fire this one. The controller consumes both
  // release orders even when an open modal blocks the screenshot itself.
  const bool screenshotActionBlocked = modalOwnsInput || buttonShortcutController.isQuickLocked();
  const auto screenshotChordResult = buttonShortcutController.updatePowerDown(
      gpio.isPressed(HalGPIO::BTN_POWER), gpio.isPressed(HalGPIO::BTN_DOWN), screenshotActionBlocked);
  if (screenshotChordResult.event == ButtonShortcutController::Event::Screenshot) {
    RenderLock lock;
    ScreenshotUtil::takeScreenshot(renderer);
  }
  if (screenshotChordResult.consumeInput) {
    return;
  }

  const bool touchscreenEscapeHatch =
      !modalOwnsInput && gpio.hasTouch() && SETTINGS.disableReaderTouchscreen && activityManager.isReaderActivity();
  const auto sideButtonShortcutResult = buttonShortcutController.updateUpDown(
      millis(), gpio.isPressed(HalGPIO::BTN_UP), gpio.isPressed(HalGPIO::BTN_DOWN), configuredSideButtonChordAction(),
      touchscreenEscapeHatch, modalOwnsInput);
  if (dispatchButtonShortcut(sideButtonShortcutResult) || sideButtonShortcutResult.consumeInput) {
    lastActivityTime = millis();
    return;
  }

  const bool powerPressed = gpio.isPressed(HalGPIO::BTN_POWER);
  const bool chordButtonPressed = gpio.isPressed(HalGPIO::BTN_UP);
  const bool shortPowerRelease = gpio.wasReleased(HalGPIO::BTN_POWER) &&
                                 gpio.getPowerButtonHeldTime() < SETTINGS.getPowerButtonLongPressDuration();
  const bool quickLockOnShortPower =
      shortPowerRelease && SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK;
  const auto shortcutResult =
      buttonShortcutController.update(millis(), powerPressed, chordButtonPressed, shortPowerRelease,
                                      quickLockOnShortPower, configuredChordAction(), modalOwnsInput);
  if (dispatchButtonShortcut(shortcutResult)) {
    lastActivityTime = millis();
    return;
  }

  if (buttonShortcutController.isQuickLocked()) {
    const bool longPowerPressed =
        powerPressed && gpio.getPowerButtonHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
    if (buttonShortcutController.tryUnlockLongPower(millis(), longPowerPressed)) {
      notifyQuickLockChanged();
      lastActivityTime = millis();
      return;
    }
    if (buttonShortcutController.tryUnlockSide(millis(), mappedInputManager.isPressed(MappedInputManager::Button::Up),
                                               mappedInputManager.wasPressed(MappedInputManager::Button::Up),
                                               mappedInputManager.wasReleased(MappedInputManager::Button::Up),
                                               SETTINGS.sideButtonUpLong != CrossPointSettings::IGNORE,
                                               mappedInputManager.isPressed(MappedInputManager::Button::Down),
                                               mappedInputManager.wasPressed(MappedInputManager::Button::Down),
                                               mappedInputManager.wasReleased(MappedInputManager::Button::Down),
                                               SETTINGS.sideButtonDownLong != CrossPointSettings::IGNORE,
                                               ReaderUtils::SKIP_HOLD_MS)) {
      notifyQuickLockChanged();
      lastActivityTime = millis();
      return;
    }
    if (handleX4ProHomeKeyQuickLockUnlock()) {
      lastActivityTime = millis();
      return;
    }
    if (activityManager.handleQuickLockUnlock(buttonShortcutController.quickLockTrigger())) {
      lastActivityTime = millis();
      return;
    }
    const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
    if (sleepTimeoutMs > 0 && buttonShortcutController.shouldQuickLockSleep(millis(), sleepTimeoutMs)) {
      LOG_DBG("SLP", "Quick Lock timeout triggered after %lu ms", sleepTimeoutMs);
      APP_STATE.quickLockResumePending = true;
      enterDeepSleep(true);
      // The simulator's deep sleep returns, unlike hardware. Keep its next
      // test loop from treating the marker as a real reboot resume.
#ifdef SIMULATOR
      APP_STATE.quickLockResumePending = false;
#endif
      lastActivityTime = millis();
    }
    mappedInputManager.clearInjectedReleases();
    // Nothing draws while locked; wait like an idle pass (ends early on input).
    // Unlock holds and unwired inputs keep the 10 ms tick.
    loopPassBlocked = true;
    InputTask::waitForInput(!InputWake::coversAllInputs() || anyInputHeld() ? 10
                                                                            : idleWaitMs(millis() - lastActivityTime));
    return;
  }

  // The entire chord is consumed until both buttons are released, so the
  // Power release cannot also run its ordinary short-press action.
  if (shortcutResult.consumeInput) return;

#ifdef SIMULATOR
  if (gpio.consumeSimulatorSleepRequest()) {
    enterDeepSleep();
    lastActivityTime = millis();
    return;
  }
#endif
  // Home-key taps are consumed until their single- or double-tap action is
  // known.
  if (handleX4ProHomeKeyShortcuts()) {
    // Simulator Home-key events bypass HalGPIO's raw touch activity signal.
    activityManager.notifyUserInput();
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs &&
      millis() - lastSleepBlockTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep(true);
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    // In the simulator, deep sleep is a no-op and returns — reset the timer so
    // the main loop does not immediately re-trigger auto-sleep.
    lastActivityTime = millis();
    return;
  }

  // Do not feed the wake gesture into getPowerButtonAction(). In particular,
  // the release edge can otherwise run the configured short/long Power action
  // in the same loop that arms the post-wake guard.
  if (!powerButtonReleasedSinceWake) {
    if (!gpio.isPressed(HalGPIO::BTN_POWER)) {
      powerButtonReleasedSinceWake = true;
    }
  } else if (millis() >= allowSleepAt) {
    const auto powerAction = getPowerButtonAction();
    if (powerAction == CrossPointSettings::SHORT_PWRBTN::QUICK_LOCK) {
      const bool longPower = gpio.getPowerButtonHeldTime() >= SETTINGS.getPowerButtonLongPressDuration();
      if (handleGlobalPowerButtonAction(powerAction,
                                        longPower ? QuickLockTrigger::LongPower : QuickLockTrigger::ShortPower)) {
        lastActivityTime = millis();
        return;
      }
    } else if (dispatchShortcutAction(powerAction)) {
      lastActivityTime = millis();
      return;
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  // The header's Wi-Fi glyph and battery percent: one ordinary repaint of the
  // current screen when the link comes or goes or the percent changes. Only
  // screens whose last frame drew a header status bar (never the reader), and
  // the percent only once input has paused; requestedFor stops a repeat if that
  // repaint shows no header.
  {
    static unsigned long lastHeaderStatusPoll = 0;
    static int requestedFor = -1;
    if (millis() - lastHeaderStatusPoll >= 1000) {
      lastHeaderStatusPoll = millis();
      const int shownWifi = BaseTheme::wifiStatusShown();
      const int shownPercent = BaseTheme::batteryPercentShown();
      const int connected = wifiHeaderBars();
      // Every 10 s: ADC boards smooth the percent on each read, so a faster
      // poll would move it (gauge reads are cached for BATTERY_POLL_MS).
      static int percent = -1;
      static unsigned long lastPercentRead = 0;
      // A mismatch re-reads first, so a frame drawn after the last read can't
      // trigger a repaint of the value it already shows.
      if (percent < 0 || millis() - lastPercentRead >= 10000 || (shownPercent >= 0 && shownPercent != percent)) {
        lastPercentRead = millis();
        percent = powerManager.getBatteryPercentage();
      }
      const bool inputPaused = millis() - lastActivityTime >= 2000;
      // Only the link coming or going repaints; bar-count changes wait for a repaint that happens anyway.
      const bool linkChanged = (shownWifi == 0) != (connected == 0);
      const bool stale = linkChanged || (inputPaused && shownPercent != percent);
      const int want = (connected > 0) << 8 | percent;
      if (shownWifi < 0 || !stale) {
        requestedFor = -1;
      } else if (requestedFor != want) {
        requestedFor = want;
        activityManager.requestUpdate();
      }
    }
  }

  // While on external power the percent climbs with no user interaction to
  // repaint it (gauge boards like the X4 Pro report SoC continuously), so poll
  // for a change once a minute. Off-charger the percent moves too slowly to
  // justify unsolicited e-ink refreshes.
#ifdef SIMULATOR
  const bool usbConnected = gpio.isUsbConnected();
#else
  const bool usbConnected = gpio.isUsbConnectedCached();
#endif
  if (usbConnected) {
    static unsigned long lastBatteryPollTime = 0UL;
    static uint16_t lastBatteryPercent = 0xFFFF;
    if (millis() - lastBatteryPollTime >= 60000UL) {
      lastBatteryPollTime = millis();
      const uint16_t percent = powerManager.getBatteryPercentage();
      if (lastBatteryPercent != 0xFFFF && percent != lastBatteryPercent) {
        activityManager.requestUpdate();
      }
      lastBatteryPercent = percent;
    }
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  kosync_auto::loop();
  // Auto sync push toast: drawn over the current screen, cleared by its next render.
  static unsigned long koSyncToastAt = 0;
  if (kosync_auto::takePushed()) {
    RenderLock lock;
    BookActions::drawToast(renderer, tr(STR_UPLOAD_SUCCESS));
    koSyncToastAt = millis() | 1;
  } else if (koSyncToastAt != 0 && millis() - koSyncToastAt >= 1500) {
    koSyncToastAt = 0;
    activityManager.requestUpdate();
  }
#if CROSSDINK_GOODIES
  goodies_remote::loop(millis() - lastActivityTime);
  knobs::loop();
#endif
#if CROSSDINK_APP_CAP_TOUCH
  // A delayed Home event is valid for this activity dispatch only. If an
  // unrelated gesture took priority, do not carry it into the next activity.
  mappedInputManager.clearDeferredHomeGesture();
#endif
  const unsigned long activityDuration = millis() - activityStartTime;

#ifdef SIMULATOR
  runSimulatorSmokeTestTick();
#endif

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity %s: %lu ms, rest of loop: %lu ms)", maxLoopDuration,
              activityManager.currentActivityName(), activityDuration, loopDuration - activityDuration);
      (void)activityDuration;
    }
  } else if (loopDuration >= 200 && millis() - lastSlowLoopLog >= 5000) {
    // Stalls below the boot's maximum, at most one line per 5 s.
    lastSlowLoopLog = millis();
    LOG_DBG("LOOP", "Slow loop: %lu ms (activity %s: %lu ms, rest of loop: %lu ms)", loopDuration,
            activityManager.currentActivityName(), activityDuration, loopDuration - activityDuration);
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), sleep one tick for faster response
  // Otherwise, use longer delay to save power
  bool skipLoopDelay = false;
  {
    // Reader scheduling inspects state also owned by the render task. Never wait
    // here: the input loop must stay available while a page is being rendered.
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      loopPassBlocked = true;
      delay(10);
      return;
    }
    skipLoopDelay = activityManager.skipLoopDelay();
  }
  loopPassBlocked = true;
  if (skipLoopDelay) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    // Not yield(): at priority 2 it only yields to tasks at 2 or above, which
    // would starve the priority-1 workers the loop is often waiting on (a
    // dictionary lookup, a prefetch) and IDLE0.
    vTaskDelay(1);
  } else {
    // Both waits end early when a key or the touch INT line changes, so the
    // longer idle tick no longer delays the first input after a pause. Screens
    // that hold the device awake for a radio exchange keep the fast tick they
    // had before, since WiFi blocks power saving anyway.
    const bool radioIdleOk = radioMayIdle();
    const bool radioExchange = activityManager.preventAutoSleep() && WiFi.getMode() != WIFI_MODE_NULL && !radioIdleOk;
    // Wi-Fi keeps the CPU at full clock unless the screen opts in (File
    // Transfer and Calibre when idle, the OPDS list, KOSync results).
    powerManager.setRadioIdleSleepAllowed(radioIdleOk);
    if (!radioExchange && millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      InputTask::waitForInput(idleWaitMs(millis() - lastActivityTime));
    } else if (radioExchange && millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // A radio exchange (Wi-Fi join, sync, download) runs on its own task and
      // the screen only polls it. At a 10 ms tick the loop cost ~11% of core 0,
      // the Wi-Fi/lwIP core, for the whole transfer.
      InputTask::waitForInput(TransferLightPulse::animating() ? TransferLightPulse::WRITE_INTERVAL_MS : IDLE_WAIT_MS);
    } else {
      // Short delay to prevent tight loop while still being responsive
      InputTask::waitForInput(10);
    }
  }
}

void loop() {
  loopPassBlocked = false;
  loopPass();
  SleepLog::loop();
  // loopTask runs on core 0 at priority 2, above IDLE0 and the priority-1
  // workers. Early returns (held chords, Home-key taps, shortcut dispatch)
  // skip the pass-end wait; one tick keeps them from starving IDLE0 into a
  // task-watchdog reset without delaying input.
  if (!loopPassBlocked) {
    vTaskDelay(1);
  }
}
