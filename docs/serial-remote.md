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
| `CMD:GAUGEINT [seconds]` | `OK:GAUGEINT started 30s (+5s baseline); result in the log as [SER] GAUGEINT done` | X4 Pro: finds the CW2017 INT_N pin. Counts falling edges by interrupt on the unused GPIOs (15-17, 45-48) for a 5 s baseline, then for `seconds` (default 30, 5-600) with every gauge alert forced on (temperature above TEMP_MAX and below TEMP_MIN, SoC alert on any 1% change), clearing the flags each second. Runs on its own task; the log line `GAUGEINT done ... flags=0x.. edges(base/armed): pin:n/n` gives the result (the INT pin has edges only while armed). Restores the gauge registers and pads. |
| `CMD:PINMON [on\|off\|status]` | `OK:PINMON on 15:<level>/<changes>/<wakes> ... chatter=0x..` | X4 Pro: passive monitor of the unused GPIOs (15-17, 45-48). Off at every boot and never saved; `on`/`off` (or Goodies > Pin monitor, which shows level, changes and wakes per pin) switch it in RAM. Inputs only (pull-up; pull-down on strapping pins 45/46), and light-sleep wake sources. Logs `[PIN] PINMON <pin> rise\|fall t=<ms>` per change (`PINMON wake pin <pin> ...` when it ended a light sleep) and `PINMON 1m changes <pin>:<n> ... chatter=0x..` each minute. A pin with more than 20 changes in a minute is released until reboot (`PINMON <pin> chatter, disabled`; chatter bit = its index in the list). `GAUGEINT` needs `PINMON off` first. |
| `CMD:SET <key> <value>` | `OK:SET <key> <value>` | Toggle, enum (raw value) or numeric setting by its web API key; saved to SD. |
| `CMD:SET get <key>` / `list [from]` | `OK:SET <key> <value> ...` | Reads a setting as `SET` takes it, plus `min= max= step=` (numbers) or `values=` (enums). `list` pages `key=value` pairs from index `from` (`next=N` while more remain). Strings are quoted; only device name, folders and the KOReader server URL (user:pass masked) are readable, other strings read `"****"`. |
| `CMD:KBDEXP <flags> [frames] [pll]` / `CMD:KBDEXP off` | `OK:KBDEXP ...` | Sets or clears a keyboard refresh override in RAM (no SD write); applied at the next keyboard open, kept until `off` or reboot. `pll` is a PLL choice (0 = panel default, 1 = 40 Hz, 2 = 50 Hz; anything else is refused) and applies with or without flag 4. |
| `CMD:KNOB list [from]` / `changed [from]` / `get <id>` / `set <id> <value>` / `reset [<id>]` | `OK:KNOB ...` | Goodies > Knobs ([goodies.md](goodies.md)). `list` pages `id=value` pairs from index `from` (`next=N` while more remain); `changed` pages only knobs off their default (the on-device `*`) as `id=value/default`, and an empty `OK:KNOB` means none; `get` adds default, min, max, step and unit; `set` clamps and snaps to the step, applies at once and saves `knobs.json`; `reset` without an id resets all and deletes the file. |
| `CMD:REFRESH [fast\|half\|full]` | `OK:REFRESH` | Re-sends the current framebuffer. |
| `CMD:HOME` | `OK:HOME` | |
| `CMD:OPEN <path>` | `OK:OPEN` | Opens a book in the reader. |
| `CMD:GOTO <screen>` | `OK:GOTO <activity>` | Opens a top-level screen 0.5 s after the reply (so a Wi-Fi reply is sent before the screen can stop the server). `<activity>` is what `ACTIVITY` reports once it is up; confirm with `WAITIDLE` + `ACTIVITY`. `ERR:GOTO:unknown_screen`, `ERR:GOTO:unavailable` (no book to resume, no OPDS server, stats off), `ERR:GOTO:busy`. |
| `CMD:GOTO list` | `OK:GOTO list <screen> ...` | Screen names in this build. |
| `CMD:SLEEP` | `OK:SLEEP` | Normal sleep flow. |
| `CMD:REBOOT` | `OK:REBOOT` | Software restart. |
| `CMD:WAITIDLE [ms]` | `OK:WAITIDLE <elapsed_ms>` or `ERR:WAITIDLE:timeout` | Replies once injected input and typing are done, no render is queued or running, no refresh is pending, and that has held for 150 ms. Default timeout 10 s. |

## Wi-Fi: POST /api/cmd

The same commands (without `CMD:`) also run over Wi-Fi, on Goodies > Wi-Fi remote and in File Transfer.
The endpoint is off until `/debug/remote-token` exists on the SD card (one line, up to 64 characters;
a bad or missing token gets `403 ERR:token`). A 6-digit PIN is enough: 5 bad tokens from one IP lock that IP out
(`429 ERR:locked`, every token endpoint) for 60 s, doubling per lockout up to 64 min, until a good token or a
reboot. Other IPs are not affected. With no token file, opening Goodies makes a random 6-digit PIN and writes it there. Goodies > API token: tap shows
the PIN or token for 10 s, a second tap offers a new PIN (old one stops working, lockouts cleared). The command runs on the main loop and the reply line is
the response body: 200 for `OK:`, 400 for `ERR:`, 404 unknown command, 503 busy or no reply within 12 s.
`PSRAMLOG` stays serial-only (use `GET /api/psram-log`). `SCREENSHOT` over Wi-Fi returns the image
(below) instead of a reply line.

`GOTO` screens: `home`, `files`, `library`, `reader` (resume last book), `settings`, `wifi` (Wi-Fi networks),
`goodies`, `transfer` (File Transfer mode picker), `transfer-wifi` (File Transfer on the saved network),
`transfer-hotspot`, `calibre`, `opds`, `nearby` (receive a book), `nearby-stats`, `usb` (USB Drive builds).
`wifi`, `transfer*`, `calibre`, `opds` and `nearby*` take the radio, so they end the Goodies Wi-Fi remote;
`transfer-wifi`, `transfer-hotspot`, `calibre` and `opds` (one server) reboot into network mode first. File
Transfer serves `/api/cmd` itself once it is on the network.

`GET` or `POST /api/screenshot` (token as for `/api/cmd`) returns the framebuffer as a binary PBM (P4, 1 = black)
in the panel's native orientation, the same frame as `TOUCH` coordinates. While the last gray pass is still on the
panel (no B/W refresh since), it returns a PGM instead (P5, maxval 255, 4 levels: 0 black, 85 dark gray, 170 light
gray, 255 white; `Content-Type: image/x-portable-graymap`). Check the first two bytes (`P4`/`P5`). The levels are
the ones sent to the panel, not a read-back of the ink. It is copied on the main task under the
render lock, so it is never half-drawn; it shows what was last drawn, even if the panel refresh is still running.

```sh
# remote-token: the PIN from Goodies > API token (tap to show), one line
curl -s --data-urlencode "token=$(cat remote-token)" --data-urlencode "cmd=KBDEXP 15 6" http://10.0.1.67/api/cmd
curl -s --data-urlencode "token=$(cat remote-token)" --data-urlencode "cmd=GOTO settings" http://10.0.1.67/api/cmd
curl -s --data-urlencode "token=$(cat remote-token)" -o screen.pnm http://10.0.1.67/api/screenshot
convert screen.pnm -rotate -90 screen.png  # portrait view (ImageMagick), as saved screenshots
```

## Wi-Fi: POST /api/ota

Flashes a firmware image with no File Transfer and no on-device confirm, on Goodies > Wi-Fi remote or File
Transfer. Same token as `/api/cmd`, sent as a header; it is checked before anything is erased.

```sh
curl -s --data-binary @firmware.bin -H "Content-Type: application/octet-stream" \
  -H "X-Token: $(cat remote-token)" http://10.0.1.67/api/ota
```

The body streams into the next OTA slot (no SD card) and is verified as it is written: size, chip, segment
table, checksum, SHA-256 and board tag, as SD Card Firmware Update does. Only a verified image switches the boot
slot; OTA rollback still applies on the next boot.

| Status | Body | Meaning |
| --- | --- | --- |
| 200 | `OK:OTA rebooting` | Verified and selected; the device restarts from the main loop about 0.2 s later. |
| 403 | `ERR:token` | Missing or wrong `X-Token`, or no `/debug/remote-token`; nothing written. |
| 400 | `ERR:OTA:<reason>` | `TOO_SMALL`, `TOO_LARGE`, `BAD_MAGIC`, `BAD_CHIP`, `WRONG_BOARD`, `BAD_SIZE`, `BAD_CHECKSUM`, `BAD_SHA`, `ERASE_FAIL`, `WRITE_FAIL`, `OTADATA_FAIL`, `READ_FAIL` (connection dropped), `OOM`. The running firmware stays selected. |

Needs `Content-Length` (curl sends it). Each 64 KiB flash erase pauses the screen briefly during the upload.

## Wi-Fi: SD file download and upload

`GET /api/download?path=<file>` and `POST /api/upload?path=<dir>` (multipart field `file`) are File Transfer's
`/download` and `/upload` behind the same token, so they also work on Goodies > Wi-Fi remote. Send the token as
the `token` query argument or an `X-Token` header; bad tokens count toward the lockout. Paths are SD-rooted
(`..` stops at `/`); hidden items and `/debug/remote-token` are refused (`403`) unless Show Hidden Files is on (the
token file always). Upload refuses an existing name (`400 File already exists: <name>`); delete first.
Both stream through a 4 KB buffer. Download honors `Range` (`206`/`416`) and upload resumes with `offset=<bytes>`
(see `webserver-endpoints.md`, `/download` and `/upload`).

```sh
curl -s -H "X-Token: $(cat remote-token)" -o book.epub "http://10.0.1.67/api/download?path=/Books/book.epub"
curl -s -H "X-Token: $(cat remote-token)" -F "file=@book.epub" "http://10.0.1.67/api/upload?path=/Books"
# resume a dropped download
curl -s -C - -H "X-Token: $(cat remote-token)" -o book.epub "http://10.0.1.67/api/download?path=/Books/book.epub"
# resumable upload: start with offset=0; after a drop, send the rest from the size the 409 reply names
curl -s -H "X-Token: $(cat remote-token)" -F "file=@book.epub" "http://10.0.1.67/api/upload?path=/Books&offset=0"
tail -c +$((N + 1)) book.epub > rest && curl -s -H "X-Token: $(cat remote-token)" -F "file=@rest;filename=book.epub" \
  "http://10.0.1.67/api/upload?path=/Books&offset=$N"
```

## Wi-Fi: live log tail

`GET /api/psram-log?since=<offset>&wait=<ms>` returns only the PSRAM log bytes after `<offset>`, with the
offset for the next poll in the `X-Log-Next` header (no token; same as the full dump without `since`).
Offsets count every byte since the ring started, so they carry across software restarts (GOTO reboots,
panics). If the ring overwrote text since the last poll the reply starts with `[psram-log gap N bytes]`; if
it restarted (power loss, deep sleep) it starts with `[psram-log restarted]` and the whole new ring. `wait`
(max 5000) holds an empty reply until new text arrives; that holds up only the web server task, so
`/api/cmd` answers after the current poll. Tail from the start of the ring (`o=0`), then follow:

```sh
o=0; while :; do n=$(curl -s --connect-timeout 3 --max-time 10 -D - -o /dev/stderr "http://10.0.1.67/api/psram-log?since=$o&wait=2000" | tr -d '\r' | awk 'tolower($1)=="x-log-next:"{print $2}'); o=${n:-$o}; done 2>&1
```
