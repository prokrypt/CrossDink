<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="./docs/images/crossdink-logo-dark.svg" />
    <img src="./docs/images/crossdink-logo.svg" alt="CrossDink logo: two ink drops" width="88" height="80" />
  </picture>
</p>

# CrossDink

> **CrossDink is a fork of [CrossInk](https://github.com/uxjulia/CrossInk)**, which is itself a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). It keeps CrossInk's fonts, reader features and reading stats, and rebuilds the firmware around the dual-core ESP32-S3 in the newer Xteink and Seeed readers.

The project started as DualPoint, a plan to put the S3's second core to work. It is now CrossDink.

### Supported Devices

CrossDink runs only on ESP32-S3 readers:

- Xteink X4 Pro
- Xteink X4 Classic
- Seeed Studio Sticky

The ESP32-C3 Xteink X3 and X4 are not supported. For those, use [CrossInk](https://github.com/uxjulia/CrossInk).

## Goals

- **Use both cores.** The screen, the reader's background work, networking and input each get their own task, so page turns and taps don't wait on indexing, image decoding or transfers.
- **Assume PSRAM.** With the ESP32-C3 limits gone, large buffers, caches and Wi-Fi buffers live in PSRAM, which keeps internal RAM free for the things that need it.
- **Spend less power while idle.** The device drops into automatic light sleep between loop ticks, and the Wi-Fi screens idle in modem sleep between transfers.
- **Write to the SD card less.** Progress, stats and session state are written only when something changed, and in small in-place slots.
- **Stay in sync with CrossInk.** Upstream CrossInk and CrossPoint changes are merged in regularly, so their reader features keep arriving here.

## What's different from CrossInk

- **Second-core work:**
  - chapters are inflated on the second core the first time they open;
  - JPEG and PNG images in books are decoded there;
  - the next page is drawn ahead there, right after a page is shown;
  - the next chapter is indexed there near the end of a chapter.
- **Input:** buttons and touch are read on their own task, woken by the input lines, so presses and swipes made while the device is busy are queued instead of lost.
- **Faster File Transfer:** File Transfer and Calibre Connect serve requests on their own task, so transfers keep flowing while the screen redraws.
- **Fewer reboots around Wi-Fi:** entering and leaving File Transfer, OPDS, KOReader Sync or Nearby Transfer no longer restarts the device when there is enough memory.
- **Light-sleep firmware:** the `x4-pro-light-sleep` build adds automatic light sleep on the X4 Pro. Buttons, touch, the frontlight and USB serial keep working.
- **Faster Library and USB Drive:**
  - the Library index is kept between visits and starts building in the background after a restart;
  - USB Drive reads ahead while it sends data.
- **Frontlight gestures:** brightness and warmth follow your finger while you drag, in 1% steps.
- **Smaller firmware:** leaner built-in TTF fonts, minified web pages and no wolfSSL debug strings save several hundred KB of flash.
- **New name and logo:** CrossDink has a two-drop logo on the boot screen and web portal. On-device data paths and file formats are unchanged, so settings, stats backups and caches carry over from CrossInk.

See [CHANGELOG.md](./CHANGELOG.md) for the full list.

## Features inherited from CrossInk

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: 12 pt, Margin: 15" /><br/>
      <em>Font: Bitter, Size: 12 pt, Margin: 15</em>
    </td>
    <td align="center">
      <img src="./docs/images/reading-stats.jpg" alt="Reading Stats with custom front button mapping shown" /><br/>
      <em>Reading Stats with custom front button mapping shown</em>
    </td>
  </tr>
</table>

- **Fonts:** Lexend Deca and Bitter as reader fonts, and Inter for the UI. Scalable TTF sizes from 8 pt to 22 pt are included, and more families can be installed from the SD card (see [SD Card Fonts](./docs/sd-card-fonts.md)).
- **Glyphs:** music notation, selected Cyrillic, and the CJK fallback ranges Project Hail Mary needs.
- **Themes:** `Minimal` and `Dashboard` themes and sleep screens.
- **Text formatting:** strikethrough, thicker underlines, `<hr>` section breaks, redaction-style text and simple tables.
- **Reader tools:** bookmarks, Focus Reading, Guide Dots, Force Paragraph Indents, auto page turn (5-120 s) and an in-book menu for reader options.
- **Reading stats and finished books:** reading stats (books finished, time, sessions, pages per minute), which can also be your sleep screen. You can mark books as finished and move them to a Read folder.
- **Sync between devices:** stats [sync](./docs/reading-stats-sync.md) and [reading progress sync](./docs/nearby-position-sync.md) between two devices.
- **Controls and Library:** remappable buttons and shortcuts (see [Controls](./docs/controls.md)), and a searchable Library with sort options.

Reader options are documented in [Reader Features](./docs/reader-features.md).

---

## Tips for the best reading experience

CrossDink runs on a microcontroller, so very large folders or complex EPUBs can be slower than they would be on a phone, tablet, or desktop app.

- Keep folders under about 200 files. For the smoothest browsing, aim for 50-100 files per folder.
- Having 1000+ books on the SD card is fine if they are split into smaller folders, such as by author, series, genre, or read/unread status.
- Avoid putting every book in the SD card root. The file browser has to scan and sort the current folder before it can show it.
- Text-first EPUBs are the best fit. Large image-heavy EPUBs, scanned books, comics, and omnibus files with thousands of sections may load slowly or fail under memory pressure.
- As a rough target, EPUBs under 20 MB tend to work the best. Files over 50 MB may still work, but they are more likely to be slow or memory-sensitive, especially if they contain many large images.
- If an EPUB is unusually slow, try [optimizing](./docs/webserver.md#epub-optimization) it with the built-in web optimizer (via File Transfer) before copying it to the SD card: remove unused high-resolution images, split very large omnibus files, and avoid embedding multiple full font families when possible.
- Use a reliable SD card and leave some free space. CrossDink stores settings, reading progress, cache files, stats, and generated book data on the card.

---

## Installation

Build and flash from source with PlatformIO (see below). The flashing and revert steps in [Installation](./docs/installation.md) also apply, as long as you use a CrossDink `.bin` built for your device.

Once CrossDink is installed, later builds can be flashed with SD Card Firmware Update in Settings, or uploaded over File Transfer.

---

## Development quick start

CrossDink uses PlatformIO for building and flashing firmware. See [Getting Started](./docs/development/getting-started.md) for prerequisites, clone setup, and validation commands.

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

Connect your device to your computer via a USB cable. Before the first build, initialize the repository's submodules (including `freeink-sdk`):

```sh
git submodule update --init --recursive
```

Then flash the firmware using the environment for your device:

| Device | Environment |
| --- | --- |
| Xteink X4 Pro | `x4-pro`, or `x4-pro-light-sleep` for automatic light sleep |
| Xteink X4 Classic | `x4-classic` |
| Seeed Studio Sticky | `sticky` |

Each has a `-debug` variant with extra logging.

```sh
pio run -e x4-pro-light-sleep --target upload
```

If PlatformIO reports `PackageException: Can not create a symbolic link for freeink-sdk/libs/hardware/BatteryMonitor, not a directory`, the `freeink-sdk` submodule is not initialized. Run the submodule command above and retry.

See [Testing and Debugging](./docs/development/testing-debugging.md) for serial logging, simulator checks, static analysis, and bug-report guidance.

---

## Notice on Contributions

CrossDink is a personal project and does not accept pull requests. Reader features that would benefit everyone belong upstream in [CrossInk](https://github.com/uxjulia/CrossInk) or [CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader).

## Credits

CrossDink is built on the work of [CrossInk](https://github.com/uxjulia/CrossInk) by uxjulia and [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader) and its contributors. If you enjoy the reader features, please consider [supporting CrossInk's development on Ko-fi](https://ko-fi.com/Q5Q01M6S7).
