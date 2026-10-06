# CrossDink — Project Map

Navigation index for agents: where things live and which file to open first.
Snapshot of 2026-10 (v1.6.0). Line counts are rough, to show which files are huge.
Rules and conventions live in `CLAUDE.md`/`AGENTS.md`; repo gotchas live in `CONTEXT.md`.

## What it is

E-reader firmware (Arduino on ESP-IDF via pioarduino) forked from CrossInk/CrossPoint.
**ESP32-S3 only** (README): Xteink X4 Pro, Xteink X4 Classic, Seeed Sticky. The C3 X3/X4
are not supported; C3 branches inherited from upstream compile out of every firmware env. Design goals: use both cores, assume PSRAM, idle in light sleep, write to SD less,
and merge upstream CrossInk often.

## Build targets (`platformio.ini`)

| Env | Device | Touch | USB Drive | SD | Light sleep | Notes |
| --- | --- | --- | --- | --- | --- | --- |
| `sticky` / `sticky-debug` | Seeed Sticky | 1 | 0 | SPI | yes | |
| `x4-pro` / `x4-pro-debug` | Xteink X4 Pro | 1 | 1 | SDMMC | yes | frontlight, Home key, `[x4_pro_net_tuning]` |
| `x4-classic` / `x4-classic-debug` | Xteink X4 Classic | 0 | 1 | SDMMC | no | buttons only, no frontlight |
| `simulator` | X3/X4-style buttons | 0 | 0 | host `./fs_` | n/a | native SDL, `-DSIMULATOR` |
| `sticky-simulator`, `x4-pro-simulator`, `x4-classic-simulator` | native profiles | match the device | | | | |

- `default_envs = sticky, x4-pro, x4-classic`. Version is `[crossdink] version`.
- Every firmware env uses the `esp32-s3-devkitc1-n16r8` board with `FREEINK_FB_PSRAM=1` and the `[dualpoint_cores]`
  SDK rebuild. Every firmware env also takes `[x4_pro_loop_core0]`, despite its name, so Arduino's `loopTask` runs on core 0.
- Light sleep comes from `[pm_autosleep]` (`CONFIG_PM_ENABLE` with tickless idle), which `[firmware_tuned]` pulls in.
  The X4 Classic envs replace that with `[sdk_tuning]` alone, so they never light-sleep.
- `x4-pro-debug` is the only env with `CROSSDINK_GOODIES` (Home > Goodies, and Knobs through `FREEINK_TUNING`)
  and `CROSSDINK_SERIAL_REMOTE` (the serial remote and PinMon). It also adds the perf, core-load and PSRAM ring logs
  and PM profiling.
- Firmware builds run `patch_wolfssl.py`, `patch_jpegdec.py`, `patch_websockets.py`, `build_web.py`,
  `build_scalable_font_assets.py`, `gen_i18n.py` and `git_branch.py` before compiling, and `check_firmware_size.py`,
  `check_app_touch_gate.py` and `rename_firmware.py` after. The simulators run only `build_scalable_font_assets.py`,
  `gen_i18n.py`, `git_branch.py` and `build_web.py`.
- The capability macros `CROSSDINK_APP_CAP_TOUCH` and `CROSSDINK_APP_CAP_USB_DRIVE` are required, and
  `include/AppCapabilities.h` checks them against the SDK's `FREEINK_CAP_*`.
  The firmware envs set `CROSSDINK_SCALABLE_FONTS=1`, as do the sticky and x4-pro simulators.
- Core pinning lives in `include/TaskCores.h`: `kUi=1` and `kWorker=0` (see Concurrency). Every firmware env
  moves `loopTask` to core 0 through `[x4_pro_loop_core0]`.

## Submodules

- `freeink-sdk/` (prokrypt/freeink-sdk): FreeInkUI, FreeInkFont, NearbyTransfer, Icons, board drivers.
  Linked through `symlink://` lib_deps. It is **not initialized in a fresh clone**; run `git submodule update --init`.
- `assets/tabler-icons/` is the icon source for `scripts/generate_icons.py`.
- The simulator runtime comes from the external lib `uxjulia/crossink-simulator`.

## `src/` — application (~120k lines)

### Core singletons and stores (top level)
- `main.cpp` (3.2k): `setup()` near :1844, then the loop, deep sleep (`enterDeepSleep` :1681),
  silent-restart routing (`silentRestartTo*`), global power/Home-key/chord shortcuts, flash-duck,
  and `[SYS]` heap/CPU logging.
- `CrossPointSettings.{h,cpp}` (`SETTINGS`, `settings.json`) and `SettingsList.{h,cpp}` (setting
  definitions, 1.1k-line header). `CrossPointState` (`APP_STATE`, `state.json`).
- `MappedInputManager` (logical buttons, gestures), `QuickActions`, `GlobalActions.h`, and `Knobs.cpp`
  (tuning knobs from `lib/Knobs/Knobs.def`, editable in Goodies > Knobs on `x4-pro-debug`).
- Stores: `BookmarkStore`, `ClippingStore`, `RecentBooksStore`, `WifiCredentialStore`,
  `OpdsServerStore`, `TtfRenderProfileStore`. Fonts: `SdCardFontSystem`, `FontInstaller`, `fontIds.h`.
- `SilentRestart.h`: `ESP.restart()` with an RTC flag, so `setup()` skips the splash and goes straight to a target
  such as OTA, OPDS or KOReader sync. It is used to clear heap fragmentation after Wi-Fi.
  `PendingOverlayResume.h` reopens the reader or frontlight drawer the user came from after that restart.

### `activities/` — screens (see `docs/activity-manager.md`)
- `Activity.h`, `ActivityManager.{h,cpp}` (1.6k): the stack, the render task (`ActivityManagerRender`), `RenderLock.h`, `ActivityResult.h`.
- `reader/` is the largest area:
  - `EpubReaderActivity.cpp` is **9.5k lines**. It starts the core-0 workers that draw the next page ahead
    (`DrawAhead`), index the next chapter (`SilentIndex`), fill the image cache and make Home thumbnails.
  - `EpubReaderDrawerActivity` (3.2k) is the touch drawer/menu, and `EpubReaderMenuModel.h` holds the menu model.
  - `TxtReaderActivity` and `XtcReaderActivity` are the other formats. `ReaderActivity` dispatches by format.
  - Dictionary: `DictionaryWordSelect`, `DictionaryDefinition`, `DictionarySuggestions`, `LookedUpWords`.
  - Clipping: `ClipSelection`, `EpubReaderClippingList`.
  - Sync: `KOReaderSyncActivity` and `KOSyncAuto` (auto sync on wake/sleep), `NearbyBookPositionSyncActivity`.
  - Stats: `BookReadingStats`, `GlobalReadingStats`, `BookStats*`, `StatsBackup`.
  - Progress: `ProgressRecord.h`, `ReaderProgressSaveDebouncer.h`, `ReaderProgressShadow`, `TwoSlotFile.h` (A/B slots).
- `home/`: `HomeActivity` (2.8k), `FileBrowserActivity`, `BookActions`, `SavedItemsHomeActivity`, `CrashActivity`.
- `library/`: `LibraryActivity` and `LibraryPrewarm`, which runs on core 0 and builds `library.idx`.
- `settings/`: `SettingsActivity` (1.7k), fonts (`FontDownload`, `FontSelection`, `TtfRenderOptions`),
  OTA (`OtaUpdate`, `SdFirmwareUpdate`), OPDS/KOReader settings, `ButtonRemap`, the clock screens, `StatusBarSettings`.
- `network/`: `WifiSelection` (1.6k), `CrossPointWebServerActivity`, `CalibreConnect`, `NearbyBookTransfer`,
  `NearbyStatsSync`, `UsbDriveActivity`, `NetworkModeSelection`.
- `browser/`: OPDS catalog (`OpdsBookBrowserActivity`, page cache/prefetch/preload pool, downloader).
- `boot_sleep/`: `BootActivity` and `SleepActivity` (1.4k; sleep covers and the image-folder index).
- `goodies/` (built only with `CROSSDINK_GOODIES`, so only in `x4-pro-debug`): `GoodiesActivity`,
  `BatteryStatsActivity`, `DisplayTestActivity` and `DisplayScript`.
- `util/`: generic screens such as keyboard entry, confirmation, option and interval selection, the frontlight panel, the BMP viewer.

### Other `src/` folders
- `components/`: `UITheme` plus `themes/` (Base, Lyra, LyraCarousel, Lyra3Covers, Minimal, RoundedRaff,
  Dashboard), `OptionPopup.h`, touch helpers (`TouchRegistry`, `TouchHeaderBackButton`), home cover caches,
  and `icons/` (generated from `*.manifest`).
- `network/`: `CrossPointWebServer.cpp` (3.7k; HTTP, WebSocket, Range), `WebDAVHandler`, `HttpDownloader`,
  OTA (`OtaUpdater`, `FirmwareFlasher`, `OtaBootSwitch`, `FirmwareBoardTag`), `SerialRemote` (`x4-pro-debug` only),
  `UsbSerialFileTransfer`, `SdWriteBehind`, `WifiBackgroundJoin`, and `html/*.generated.h` (generated, do not edit).
- `platform/`: `InputTask` (buttons and touch on their own task, core 0), `InputWake`, `PinMon` (`x4-pro-debug` only),
  USB/JTAG handoff.
- `util/`: dictionary engine (`Dictionary`, `DictionaryLookupController`/`Worker`, `DictLayout`), battery
  logs and estimates, `BookCacheUtils`, `WorkerTask` (pinned-task helper), `WordSelectNavigator`,
  `TransferLightPulse`, `FrontlightSchedule`, `DaylightSaving`, `ScreenshotUtil`, `SleepLog`, `BootReason`.
- `clippings/`: clip text building and highlight matching. `simulator/`: `SimulatorSmokeTest.cpp`, the home-key shim.

## `lib/` — local libraries

| Lib | Role |
| --- | --- |
| `hal/` | App HAL: `HalStorage` (the `/.crossdink` and `/.crosspoint` twin overlay), `HalDisplay`, `HalGPIO`, `HalPowerManager`, `HalFrontlight`, `HalClock`, `HalTiltSensor`, `HalSpiBus`, USB-drive read-ahead |
| `Epub/` | `Epub.{h,cpp}` plus `Epub/`: `parsers/` (container, OPF, NCX/NAV, `ChapterHtmlSlimParser`), `css/`, `blocks/`, `Page`, `Section` (`sections/N.bin`), `ParsedText` (line breaking), `tables/`, `hyphenation/` (Liang; `generated/` tries), `converters/` (JPEG/PNG decoding to the framebuffer, `DecodePipeline` on core 0), `image/PxcV2`, `HtmlInflateStream` (inflate on core 0), `BookMetadataCache` (`book.bin`) |
| `GfxRenderer/` | Renderer, bitmaps, `FontCacheManager` |
| `EpdFont/`, `ScalableFont/` | Built-in and SD-card bitmap fonts, catalog index; scalable TTF through FreeInkFont |
| `Txt/`, `Xtc/` | TXT and XTC formats |
| `KOReaderSync/` | Sync client, document ID, XPath-to-chapter mapping, credentials |
| `LibraryIndex/` | `library.idx` builder and format |
| `OpdsParser/`, `JsonParser/` | Streaming OPDS XML and JSON parsing (release JSON) |
| `Serialization/` | `PersistableStore` (writes `path.tmp`, keeps `path.bak` until the rename lands), `BufferedFile`, obfuscation |
| `Memory/`, `MemoryBudget/` | `makeUniqueNoThrow`, `Arena`, `BuildScratch`, budgets |
| `Logging/` | `LOG_*` including `[WRN]`, `PerfLog`, PSRAM ring log |
| `I18n/` | `translations/*.yaml` (28 languages) produce generated keys and strings |
| Others | `ZipFile`, `miniz`, `uzlib`, `InflateReader`, `expat`, `XmlParserUtils`, `Utf8`, `MiniBidi`, `FsHelpers`, `FileIndex`, `DictHtmlRenderer`, `Jpeg/PngToBmpConverter`, `Knobs`, `AppVersion`, `HalClockSim` |

## Concurrency (FreeRTOS tasks)

| Core | Tasks |
| --- | --- |
| Core 1 (`kUi`) | `ActivityManagerRender`, `WebServer` (PSRAM stack when possible), `FwRead`, `usbReadAhead` |
| Core 0 (`kWorker`) | Arduino `loopTask` (setup, input polling, activity loop), `Input`, the Wi-Fi driver and lwIP, `DrawAhead` and `SilentIndex` (PSRAM stacks), `HtmlInflate`, `ImgDecode`, `ImageCache`, `HomeThumbs`, `HomeCovers`, `LibPrewarm`, `DictLookup`, `WsWriter` and `HttpWriter` (`SdWriteBehind`) |
| Caller's core | `OtaFlash` |
| Unpinned, `x4-pro-debug` only | `pinmon`, `gaugeint` (serial remote) |

## On-SD data (`docs/data-cache.md`, `docs/file-formats.md`)

`/.crossdink/` holds:
- Settings and session JSON: `settings.json`, `state.json`, `recent.json`, `wifi.json`, `opds.json`, `koreader.json`, `knobs.json` (only when a knob is off its default).
- Global reading stats: `global_stats_dink.bin` and `.bak` (two slots; legacy `global_stats.bin` is read until the first save).
- `library.idx`, plus the `bookmarks/` and `clippings/` folders.
- One cache folder per book:
  - EPUB: `epub_<fnv64>/`, with `book.bin`, `sections/*.bin`, `progress.bin` (A/B slots), `stats_v5.bin`, `reader_settings.bin`, covers and thumbnails, and `path.txt`.
  - Other formats: `xtc_<hash>/` and `txt_<hash>/`, still keyed by path.

## Web portal, tests, tooling

- Web: `web/templates/base.html` with `web/pages/{home,files,fonts,settings,logs}.{html,css,js}` and
  `web/assets/` are compiled by `scripts/build_web.py`. `scripts/preview_web.py` previews them.
  Endpoints are listed in `docs/webserver-endpoints.md`.
- Tests: 79 native CMake/CTest targets in `test/<name>/`. Run them with
  `cmake -S test -B build/test -G Ninja && cmake --build build/test && ctest --test-dir build/test`.
  The CI job installs `cmake ninja-build libexpat1-dev zlib1g-dev`. Smoke test: `scripts/run_simulator_smoke_test.py`.
- CI (`.github/workflows/`): `ci.yml`, `release.yml`, `release_candidate.yml`, `release-fonts.yml`,
  `pages.yml` (builds the `site/` website and deploys `site/dist`; runs when `docs/` or `site/` change),
  `upstream-ref-check.yml`, `issue-triage.yml`.
- Post-build checks on every firmware build: `scripts/check_firmware_size.py` (size guard) and
  `scripts/check_app_touch_gate.py`, which fails a button-only build that still links touch symbols.
- Docs to read before deeper work:
  - `docs/development/architecture.md`
  - `docs/activity-manager.md`
  - `docs/epub-indexing.md`
  - `docs/epub-render-modes.md`
  - `docs/scalable-fonts.md` and `docs/sd-card-fonts.md`
  - `docs/nearby-*.md`
  - `docs/serial-remote.md`
  - `docs/touch-navigation.md`
  - `docs/simulator.md`
