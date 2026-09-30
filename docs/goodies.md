# Goodies (debug builds)

`x4-pro-debug` builds (`-DCROSSDINK_GOODIES=1`) add **Goodies** to the Home menu.

- **Display test** lists the built-in tests (`src/activities/goodies/DisplayScript.cpp`), then every
  `/debug/display/*.txt` on the SD card, sorted by file name. New tests need no reflash: drop a file in
  that folder.
- **Wi-Fi remote** (value column: `OFF`, `Connecting...` or the device IP) joins the last used saved network in
  the background (the Wi-Fi picker opens only when none is saved), then keeps Wi-Fi up with a log-only web
  server: `GET /api/psram-log` (also
  `http://crosspoint.local/api/psram-log`), plus the token-gated `POST /api/cmd` from
  [serial-remote.md](serial-remote.md#wi-fi-post-apicmd). It has no file, settings, upload or `/api/status` routes, so
  nothing touches the SD card or the I2C bus behind other screens. Tap again to turn Wi-Fi off. The toggle is
  saved in `crossink-settings.json` (`goodiesWifiRemote`), written only when it changes. Wi-Fi screens (File
  Transfer, Calibre Connect, OPDS, Nearby) take the radio while open; afterwards, and after every sleep wake,
  restart or power-on, the remote rejoins in the background: 5 s after boot, once input has paused for 2 s,
  retrying after 1, 2, 4... up to 10 min when the network is out of reach. The join (reading `wifi.json`,
  starting the Wi-Fi driver) runs on its own task, so input and drawing never wait for it (logged as
  `join task N ms`). With no saved network, turning it on opens the Wi-Fi picker. While the idle server waits
  for requests, the device power saves as File Transfer does (light sleep with modem sleep); Wi-Fi still costs battery.

Back stops a running test; Back again leaves the result screen.

## Line script

One command per line. Blank lines and lines starting with `#` are ignored. Coordinates are logical
(the current orientation); keep them inside 480x480 to fit both orientations. Files over 8 KB or 256
commands are rejected, and the result screen shows the failing line.

| Command | Effect |
| --- | --- |
| `name <text>` | Title for the log and result screen (default: file name). |
| `fill white` / `fill black` | Clear the framebuffer. |
| `pattern checker N` | Checkerboard of N px cells over the current buffer. |
| `pattern hstripes N` / `pattern vstripes N` | Black stripes N px wide, N px apart. |
| `pattern text` | Sample text lines over the whole screen. |
| `box X Y W H [white]` | Filled rectangle (black unless `white`). |
| `invert` | Invert the whole framebuffer. |
| `label A \| B \| C` | White band across the top with up to 3 parts (A bold), word-wrapped to the width, shown by the next refresh. Convention: what this is \| what to look for \| what is next. |
| `refresh full\|half\|fast\|du` | Show the framebuffer. `du` is a Fast refresh with the keyboard's balanced DU LUT (compile-time DC-balance checked); the others run the panel's OTP waveforms. |
| `frames N` | DU LUT frames (1..63, default 6). |
| `pll 0xNN` | PLL (0x30) value during DU refreshes; `0` keeps the default. |
| `probe n2ocp` | The next DU refresh re-runs itself 1.5 s later with no OLD resync between (UC8179 N2OCP check; a blink = no copy). |
| `scrub half\|du` | The next Fast/DU refresh runs as a Half scrub, or a DU scrub (DU needs `refresh du`). |
| `wait MS` | Pause (0..60000 ms). |
| `repeat N` ... `end` | Loop N times (1..1000, nesting up to 4). |
| `text X Y <text>` | Draw bold text at X,Y. |
| `pick X Y CW CH COLS ROWS \| question \| name1 \| ...` | Wait for a tap on one cell of a COLS x ROWS grid of CW x CH cells at X,Y (or Left/Right then Confirm); logs the square number and its name. |
| `note <text>` | Write a line to the log. |
| `ask <question> \| A \| B` | Show the question over the test image; Left/left half = A, Right/right half = B. The answer is logged. |

`frames` and `pll` stay in effect until changed. `window` and `resync` were removed: both could leave the
controller's OLD plane unlike the panel, so the next refresh would drive pixels one way.

## Log lines

Every line is tagged `[GDY]` and starts with `test="<name>"`:

```
test="Moving box" start ops=41
test="Moving box" n=3 mode=fast upload=38 drf=262 sync=31 rows=480 total=335 frames=6 pll=0x00
test="Moving box" note full
test="Moving box" ask="After 12 Fast moves: gray boxes where it had been?" answer="No"
test="Moving box" done refreshes=14
```

`upload`, `drf` and `sync` are the UC8179 driver's own ms counters (-1 when the driver did not measure
that refresh); `total` is the whole refresh as the app saw it. The result screen shows the per-mode
averages and the answers.

## Example: `/debug/display/ghost-stripes.txt`

```
name Stripes ghosting
fill white
refresh half
repeat 5
pattern hstripes 8
refresh fast
fill white
refresh fast
end
ask Stripes left behind? | Yes | No
```
