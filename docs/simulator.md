---
title: Simulator
nav_order: 15
---

# Development Device Simulator

CrossDink can run in the [CrossDink simulator](https://github.com/prokrypt/crossdink-simulator), a fork of the [CrossInk simulator](https://github.com/uxjulia/crossink-simulator) that tracks CrossDink's HAL. It renders the e-ink display in an SDL2 window. Use it for quick sanity checks without flashing firmware every time.

`platformio.ini` pins the simulator by commit, so a simulator change only reaches CrossDink when that pin moves.

## Platform Support

The simulator builds on macOS (Intel and Apple Silicon) and Linux. SDL flags come from `sdl2-config`; on Linux the simulator library also links OpenSSL's `libcrypto` for MD5. Native Windows is not supported; use WSL.

## Prerequisites

```sh
# macOS
brew install sdl2

# Linux (Debian/Ubuntu)
sudo apt install libsdl2-dev libssl-dev
```

## Setup

Place EPUB books in `./fs_/books/` relative to the project root. That maps to the SD-card `/books/` path on device.

## Build And Run

```sh
pio run -e simulator
.pio/build/simulator/program
```

Use the X4 Pro environment to enable its touch, frontlight, and Home-key behavior:

```sh
pio run -e x4-pro-simulator -t run_simulator
```

`sticky-simulator` and `x4-classic-simulator` run the Sticky and X4 Classic profiles the same way.

## Smoke Test

`scripts/run_simulator_smoke_test.py` builds an environment, runs it headless against an isolated `fs_`, and walks Home, Library, File Browser, the reader, its menus and Settings:

```sh
python3 scripts/run_simulator_smoke_test.py --env x4-pro-simulator
```

## Working On The Simulator

To build against a local checkout of the simulator next to this repo (`../crossdink-simulator`), override `[simulator-base]` in an uncommitted `platformio.local.ini`: point `lib_deps` at `simulator=symlink://../crossdink-simulator` and the last `extra_scripts` entry at `post:../crossdink-simulator/run_simulator_project.py`, keeping the other entries as they are in `platformio.ini`. When the change works, push it to the simulator repository and move the pin in `platformio.ini` to that commit.

## Keyboard Controls

| Key | Action |
| --- | --- |
| Up / Down | Page back / forward (side buttons) |
| Left / Right | Left / right front buttons |
| Return | Confirm / Select |
| Escape | Back |
| P | Power |
| H | X4 Pro Home key (tap to go Home; hold for 700 ms to toggle the reader menu) |

The `H` mapping is active only in `x4-pro-simulator`.

## Cache Note

On first open of an EPUB, an **Indexing...** popup appears while the section cache is built in `.crossdink/`.

If rendering looks stale after a code change, delete `./fs_/.crossdink/` to clear simulator caches.
