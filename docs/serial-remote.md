# Serial remote control

Debug builds only (`-DCROSSDINK_SERIAL_REMOTE=1`, set in `x4-pro-debug`).
Commands are single lines on the USB serial port, handled by `src/network/SerialRemote.cpp`
through the existing `CMD:` channel in `UsbSerialFileTransfer`. Lines are at most 255 bytes.

Every command gets exactly one reply line, `OK:<VERB> ...` or `ERR:<VERB>:<reason>`
(`WAITIDLE` replies when idle or on timeout). Log lines share the port, so hosts should
match only lines that start with `OK:`, `ERR:`, `SCREENSHOT_` or `PSRAMLOG_`. Commands
run on the main loop, which pauses while the device is quick-locked.

Coordinates are framebuffer pixels in the panel's native orientation, the same frame as
the `CMD:SCREENSHOT` dump (`CMD:FBINFO` gives the size).

| Command | Reply | Notes |
| --- | --- | --- |
| `CMD:PING` | `OK:PING` | |
| `CMD:KEY <button> [tap\|down\|up] [ms]` | `OK:KEY` | back, confirm, left, right, up, down, power. Raw hardware keys, so side-button layout mapping still applies. `tap` holds 80 ms by default. |
| `CMD:TOUCH <x> <y> [ms]` | `OK:TOUCH` | Tap (default 60 ms). Use about 800 ms for a long press. |
| `CMD:SWIPE <x0> <y0> <x1> <y1> [ms]` | `OK:SWIPE` | Linear drag, default 300 ms. |
| `CMD:TYPE <text>` | `OK:TYPE <bytes>` | One UTF-8 character per rendered frame into the foreground text entry. `ERR:TYPE:no_text_input` if there is none. |
| `CMD:SCREENSHOT` | `SCREENSHOT_START:<bytes>`, raw 1-bit framebuffer, `SCREENSHOT_END` | Existing command. |
| `CMD:FBINFO` | `OK:FBINFO <w> <h> <bytes>` | |
| `CMD:STATUS` | `OK:STATUS {json}` | Activity, uptime, render idle, framebuffer size, heap, chip temperature. The full status is `/api/status` over Wi-Fi. |
| `CMD:ACTIVITY` | `OK:ACTIVITY <name>` | |
| `CMD:HEAP` | `OK:HEAP internal_free=.. internal_min=.. internal_largest=.. psram_free=.. psram_largest=..` | |
| `CMD:SET <key> <value>` | `OK:SET <key> <value>` | Toggle, enum (raw value) or numeric setting by its web API key; saved to SD. |
| `CMD:KBDEXP <flags> [frames] [pll]` / `CMD:KBDEXP off` | `OK:KBDEXP ...` | Sets or clears a keyboard refresh override in RAM (no SD write); applied at the next keyboard open, kept until `off` or reboot. |
| `CMD:REFRESH [fast\|half\|full]` | `OK:REFRESH` | Re-sends the current framebuffer. |
| `CMD:HOME` | `OK:HOME` | |
| `CMD:OPEN <path>` | `OK:OPEN` | Opens a book in the reader. |
| `CMD:SLEEP` | `OK:SLEEP` | Normal sleep flow. |
| `CMD:REBOOT` | `OK:REBOOT` | Software restart. |
| `CMD:WAITIDLE [ms]` | `OK:WAITIDLE <elapsed_ms>` or `ERR:WAITIDLE:timeout` | Replies once injected input and typing are done, no render is queued or running, no refresh is pending, and that has held for 150 ms. Default timeout 10 s. |
