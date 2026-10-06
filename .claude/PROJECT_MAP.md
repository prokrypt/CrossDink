# CrossDink — Project Map

Navigation index for agents: where things live and which file to open first.
Snapshot of 2026-10 (v1.6.0, `0b98add`). Line counts are rough, to show which files are huge.
Rules and conventions live in `CLAUDE.md`/`AGENTS.md`; repo gotchas live in `CONTEXT.md`.

## What it is

E-reader firmware (Arduino on ESP-IDF via pioarduino) forked from CrossInk/CrossPoint.
**ESP32-S3 only** (README): Xteink X4 Pro, Xteink X4 Classic, Seeed Sticky. The C3 X3/X4
are not supported. That makes the `default` env and the C3 notes in `CLAUDE.md`/`AGENTS.md`
out of date. Design goals: use both cores, assume PSRAM, idle in light sleep, write to SD less,
and merge upstream CrossInk often.

## Build targets (`platformio.ini`)

| Env | Device | Touch | USB Drive | Notes |
| --- | --- | --- | --- | --- |
| `sticky` / `sticky-debug` | Seeed Sticky | 1 | 0 | PSRAM framebuffer |
| `x4-pro` / `x4-pro-debug` | Xteink X4 Pro | 1 | 1 | light-sleep PM, loopTask on core 0, frontlight, Home key |
| `x4-classic` / `-debug` | Xteink X4 Classic | 0 | 1 | buttons only, no frontlight |
| `simulator` | X3/X4-style buttons | 0 | 0 | native SDL, `-DSIMULATOR` |
| `sticky-simulator`, `x4-pro-simulator`, `x4-classic-simulator` | native profiles | match the device | | |

- `default_envs = sticky, x4-pro, x4-classic`. Version is `[crossdink] version`.
- Shared sections: `[base]`, `[firmware_tuned]`, `[sdk_tuning]`, `[pm_autosleep]`,
  `[dualpoint_cores]`, `[x4_pro_net_tuning]`, `[x4_pro_loop_core0]`, `[x4_pro_tinyusb_prebuilt]`.
- Pre-build scripts: `gen_i18n.py`, `build_web.py`, `build_scalable_font_assets.py`, `git_branch.py`.
- The capability macros `CROSSDINK_APP_CAP_TOUCH` and `CROSSDINK_APP_CAP_USB_DRIVE` are required, and
  `include/AppCapabilities.h` checks them against the SDK's `FREEINK_CAP_*`.
  `CROSSDINK_SCALABLE_FONTS=1` on S3 devices.
- Core pinning lives in `include/TaskCores.h`: `kUi=1` runs the render task, `kWorker=0` runs radio and background workers.

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
- `MappedInputManager` (logical buttons, gestures), `QuickActions`, `GlobalActions.h`, `Knobs.cpp`
  (tuning knobs from `lib/Knobs/Knobs.def`).
- Stores: `BookmarkStore`, `ClippingStore`, `RecentBooksStore`, `WifiCredentialStore`,
  `OpdsServerStore`, `TtfRenderProfileStore`. Fonts: `SdCardFontSystem`, `FontInstaller`, `fontIds.h`.
- `SilentRestart.h` and `PendingOverlayResume.h` hold the reboot-into-target handoff.

### `activities/` — screens (see `docs/activity-manager.md`)
- `Activity.h`, `ActivityManager.{h,cpp}` (1.6k): the stack, the render task (`ActivityManagerRender`), `RenderLock.h`, `ActivityResult.h`.
- `reader/` is the largest area:
  - `EpubReaderActivity.cpp` is **9.5k lines** and owns the background workers (inflate, image cache, home thumbs).
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
- `goodies/`: `GoodiesActivity`, `BatteryStatsActivity`, `DisplayTestActivity` and `DisplayScript`.
- `util/`: generic screens such as keyboard entry, confirmation, option and interval selection, the frontlight panel, the BMP viewer.

### Other `src/` folders
- `components/`: `UITheme` plus `themes/` (Base, Lyra, LyraCarousel, Lyra3Covers, Minimal, RoundedRaff,
  Dashboard), `OptionPopup.h`, touch helpers (`TouchRegistry`, `TouchHeaderBackButton`), home cover caches,
  and `icons/` (generated from `*.manifest`).
- `network/`: `CrossPointWebServer.cpp` (3.7k; HTTP, WebSocket, Range), `WebDAVHandler`, `HttpDownloader`,
  OTA (`OtaUpdater`, `FirmwareFlasher`, `OtaBootSwitch`, `FirmwareBoardTag`), `SerialRemote`,
  `UsbSerialFileTransfer`, `SdWriteBehind`, `WifiBackgroundJoin`, and `html/*.generated.h` (generated, do not edit).
- `platform/`: `InputTask` (buttons and touch on their own task, core 0), `InputWake`, `PinMon`, USB/JTAG handoff.
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
| `Serialization/` | `PersistableStore` (atomic `.bak` writes), `BufferedFile`, obfuscation |
| `Memory/`, `MemoryBudget/` | `makeUniqueNoThrow`, `Arena`, `BuildScratch`, budgets |
| `Logging/` | `LOG_*` including `[WRN]`, `PerfLog`, PSRAM ring log |
| `I18n/` | `translations/*.yaml` (28 languages) produce generated keys and strings |
| Others | `ZipFile`, `miniz`, `uzlib`, `InflateReader`, `expat`, `XmlParserUtils`, `Utf8`, `MiniBidi`, `FsHelpers`, `FileIndex`, `DictHtmlRenderer`, `Jpeg/PngToBmpConverter`, `Knobs`, `AppVersion`, `HalClockSim` |

## Concurrency (FreeRTOS tasks)

| Core | Tasks |
| --- | --- |
| Core 1 (UI) | `ActivityManagerRender`, `FwRead` |
| Core 0 (worker) | `Input`, `HtmlInflate`, `ImgDecode`, `ImageCache`, `HomeThumbs`, `HomeCovers`, `LibPrewarm`, `DictLookup`, `WebServer` (PSRAM stack when possible), `SdWriteBehind`, `OtaFlash` |
| Unpinned | `pinmon`, `gaugeint` (debug tools) |

## On-SD data (`docs/data-cache.md`, `docs/file-formats.md`)

`/.crossdink/` holds:
- Settings and session JSON: `settings.json`, `state.json`, `recent.json`, `wifi.json`, `opds.json`, `koreader.json`.
- `library.idx`, plus the `bookmarks/` and `clippings/` folders.
- One cache folder per book:
  - EPUB: `epub_<fnv64>/`, with `book.bin`, `sections/*.bin`, `progress.bin` (A/B slots), `stats_v5.bin`, `reader_settings.bin`, covers and thumbnails, and `path.txt`.
  - Other formats: `xtc_<hash>/` and `txt_<hash>/`, still keyed by path.

## Web portal, tests, tooling

- Web: `web/templates/base.html` with `web/pages/{home,files,fonts,settings,logs}.{html,css,js}` and
  `web/assets/` are compiled by `scripts/build_web.py`. `scripts/preview_web.py` previews them.
  Endpoints are listed in `docs/webserver-endpoints.md`.
- Tests: about 75 native CMake/CTest targets in `test/<name>/`. Run them with
  `cmake -S test -B build/test -G Ninja && cmake --build build/test && ctest --test-dir build/test`.
  The CI job installs `cmake ninja-build libexpat1-dev zlib1g-dev`. Smoke test: `scripts/run_simulator_smoke_test.py`.
- CI (`.github/workflows/`): `ci.yml`, `release.yml`, `release_candidate.yml`, `release-fonts.yml`,
  `pages.yml` (deploys `docs/` and `site/`), `upstream-ref-check.yml`, `issue-triage.yml`.
- Size guard: `scripts/check_firmware_size.py`. Touch gating check: `scripts/check_app_touch_gate.py`.
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
