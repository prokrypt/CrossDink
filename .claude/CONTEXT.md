# CrossPoint Reader — Durable Context

Keep this file focused on repo-specific gotchas that are worth reusing in future sessions.
For a directory/file navigation index (targets, subsystems, tasks, SD layout), read `.claude/PROJECT_MAP.md`.

## Targets

- CrossDink is ESP32-S3 only (Sticky, X4 Pro, X4 Classic). There is no `default`/C3 env; `CLAUDE.md`/`AGENTS.md` C3 notes are stale. Firmware envs: `sticky`, `x4-pro`, `x4-classic` (+ `-debug`).
- `freeink-sdk` is not checked out in a fresh clone; run `git submodule update --init freeink-sdk` before building or reading SDK code.

## FreeInk SDK

Refer to https://freeink.org/llms.txt for guidance.

## Simulator

- Simulator patches belong in the adjacent `crossink-simulator` repo.
- The valid local simulator env in this repo is `simulator`, and `pio run -e simulator` currently builds cleanly.
- Known simulator limits:
  - Images: `[simulator-base]` now builds the real `PNGdec` and `JPEGDEC` with `-DCROSSPOINT_SIM_USE_NATIVE_DECODERS`
    and ignores only `hal` and `WebSockets`. Older notes said the decoders were stubs that always fail
    (`JPEGDEC fallback: open failed (err=-1)`); check in a simulator run before relying on either behavior.
  - `esp_deep_sleep_start()` is a no-op in simulator.
  - `HalStorage` uses POSIX file access under `./fs_` and allows multiple readers, unlike real hardware.

## Real Hardware / Storage

- SdFat on hardware allows only one open reader per file path at a time. If a fallback needs to reopen the same file, close the first handle before reopening.

## Rendering / Reader Pipeline

- `ImageBlock::render()` (`lib/Epub/Epub/blocks/ImageBlock.cpp`) draws images in the BW and grayscale passes; only when
  `DirectPixelWriter::bwImages` is on do the gray planes leave images out.
- Kindle EPUBs may contain paired high-res and old-Kindle fallback images. `ChapterHtmlSlimParser` should skip `<img>` nodes with `data-AmznRemoved-M8` to avoid duplicate stacked images.
- After image/layout pipeline changes that affect cached EPUB output, clear the affected `.crossdink/epub_<hash>/` cache if behavior looks stale.

## UI Consistency

- Use FreeInkUI SDK components and input routing for list-style screens where possible. Row rendering, touch targets,
  hit testing, and pagination should share the same FreeInkUI list configuration instead of custom touch scaling.

## Heap Baselines (X4 hardware, SD card font)

"X4 hardware" most likely means the ESP32-C3 X4, which CrossDink no longer supports. Treat these numbers as rough
guides on S3 devices.

- A normal resume-into-partial reading session runs at ~85-90KB free / ~49KB maxAlloc by
  the first watermark crossing (Epub metadata + x-locations + resident glyph caches).
  Do not read mid-range heap numbers as session degradation without checking the scenario.
- SD-font section builds cost ~38-50KB at cold start; the 4-style advance-table prewarm
  (~30KB incl. 16KB contiguous scratch) dominates. The idle SD-font prewarm is now gated by the knobs
  `idlePrewarmMinFree` (64 KB) and `idlePrewarmMinBlock` (40 KB) in `lib/Knobs/Knobs.def`.

## Misc Repo Gotchas

- Time zones are not POSIX TZ strings. The RTC runs in UTC, the setting is a quarter-hour offset biased by 48
  (48 = UTC+0), and `src/util/LocalClock` with `src/util/DaylightSaving.h` adds daylight saving.
- `MinimalTheme::drawHeader()` (also used by `DashboardTheme`) does not call `BaseTheme::drawHeader()`, so header
  changes in the base theme must be duplicated there. Lyra inherits the base header, and RoundedRaff calls it.
