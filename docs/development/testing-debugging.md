---
title: Testing & Debugging
parent: Development
nav_order: 4
---

# Testing and Debugging

CrossDink runs on real hardware, so debugging usually combines local build checks, simulator checks, and on-device logs.

## Local checks

Make sure `clang-format` 21+ is installed and available in `PATH` before running the formatting step.
If needed, see [Getting Started](./getting-started.md).

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run -e simulator
pio run -e x4-pro
```

Replace `x4-pro` with `x4-classic` or `sticky` for those devices. `pio run` without `-e` builds all three firmware targets (`default_envs` in `platformio.ini`). Use it for a comprehensive build check, but prefer explicit environments while iterating.

## Flash and monitor

Flash firmware:

```sh
pio run -e x4-pro --target upload
```

Open serial monitor:

```sh
pio device monitor
```

Optional enhanced monitor:

```sh
python3 -m pip install pyserial colorama matplotlib
python3 scripts/debugging_monitor.py
```

## Useful bug report contents

- Firmware version and build environment
- Exact steps to reproduce
- Expected vs actual behavior
- Serial logs from boot through failure
- Whether issue reproduces after clearing the affected book cache or using **Clear Reading Cache**

## Common troubleshooting references

- [Common Issues](../troubleshooting.md)
