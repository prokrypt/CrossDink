## [Unreleased]

### Added
- Debug builds (X4 Pro): Goodies > Knobs lists internal timing, threshold and frame-count constants on tabs like Settings (Panel, Light, Touch, Power, Heap, Book, Wi-Fi; long-press Up/Down or tap to switch) with their units, and changes each one with a slider; changed rows show a *. Changes apply at once and are saved to `/.crosspoint/knobs.json` (only values that differ from the default); Reset all goes back to the defaults and deletes the file. If the device crashes 3 times without staying up 30 s, the file is set aside as `knobs.bad.json` and the defaults are used; holding Back while it starts ignores the file for that boot. Memory gates can only be raised. Also over USB serial and the Wi-Fi remote: `KNOB list [from]`, `KNOB get <id>`, `KNOB set <id> <value>`, `KNOB reset [<id>]`. Display frame counts only feed the charge-balanced waveform generators; waveform shapes, voltages and VCOM are not knobs. Release builds keep the constants and are unchanged.
- Web portal: a Logs page (nav link on every page) shows `/api/status`, the PSRAM log (debug builds) and every file under `/debug/` on the SD card (battery logs, crash report, display scripts). Pick a source to view it, type to filter its lines, or download it. Sources a build or card lacks are left out.
- X4 Pro (debug builds): battery log. Battery % changes, charger connect/disconnect and charge complete, boot (with reset reason), deep sleep and wake, firmware updates, Wi-Fi on/off, File Transfer/Calibre/USB Drive start and end, and frontlight level changes each add a row (time, %, mV, charging, USB, battery temperature, light level) to a 64 KB buffer in PSRAM that survives restarts and crashes. It is written to `/debug/logs/battery.csv` on the SD card before every sleep, after a boot, when the buffer is 3/4 full or the battery is at 5% or less, always after 2 s without input; at 256 KB the file becomes `/debug/logs/battery.1.csv`.
- X4 Pro (debug builds): Goodies > Battery & stats shows a battery % graph from that log (a bar marks time asleep), drain per hour awake and asleep since the last unplug with an estimate of time left, wake/boot counts, awake and asleep time, refresh counts by kind, pages read and reading time, temperatures, uptime, reset and wake reasons, memory and the firmware sha. Confirm resets the counters (the log file stays).
- X4 Pro (debug builds): Goodies > Display test > Panel conditioning. After a confirm, about 30 s of balanced black/white swings with null discharges in between, ending on white, to even out charge left on the panel. Back stops it between swings.
- X4 Pro: Display > Frontlight > Dim Light on Flash (off by default). Whenever a refresh flashes the screen (Half/Full, anti-aliased pages in Sharpflash, Softfast menus and gray exits (not its page turns, which no longer flash), full-screen scrubs), a lit frontlight fades out just as the flash shows and fades back up to your level over 0.3 s when the refresh ends. The display driver times it for each waveform, so on anti-aliased pages the light stays up for the first half of the refresh, before the background moves. Flash Dim Level (0-90% in 10% steps, default 0% = dark) sets how far it dims, as a share of your brightness. Each kind of refresh has its own timing, set from its waveform: on anti-aliased page turns the light is dark from the start of the refresh and back up about 200 ms before it ends; on Half/Full refreshes and on menus or the drawer over an anti-aliased page it is dark from the start and back up as the refresh ends. Debug builds tune each kind in Goodies > Knobs: flashGray/Full/Paint DimMs and RestoreMs (-800 to +800 ms, negative is earlier; the dim never starts before the refresh does) take effect on the next flash, and flashDimLevel there is the same Flash Dim Level setting; neither ever delays a refresh. It never turns on a light that was off and saves nothing to the SD card.
- X4 Pro: Text Anti-Aliasing has three choices: Off, Sharpflash (one full-screen flash per anti-aliased page, the previous behavior) and Softfast (no full-screen flash on text pages: the page shows in black and white first, then only its light-gray edge pixels turn gray, for bolder text; slower; image pages, the first page after opening a book and the first page after a menu refresh like Sharpflash). The in-reader menu offers the same three choices. Other devices treat Softfast like Sharpflash.
- X4 Pro: Display > Frontlight > Light Timeout (1 / 2 / 5 / 10 min / Never, default Never). With no input for that long, the frontlight fades out over 1 s. The next key or touch brings it back at once and does nothing else. It never turns on a light that was off, and it waits while File Transfer, Calibre or USB Drive keep the device awake.
- Debug builds (X4 Pro): a Goodies entry on Home. Its Display test menu runs built-in refresh tests (modes, ghosting, DU frames, windowed upload, scrubs) and any line-script tests placed in `/debug/display/` on the SD card, logs each refresh's timings, and can ask a Yes/No style question and log the answer. Format: `docs/goodies.md`.
- Debug builds (X4 Pro): Goodies > Wi-Fi remote joins a saved Wi-Fi network and keeps it up in the background, serving the PSRAM log (`/api/psram-log`) from any screen without opening File Transfer. The row shows the device's address; tap again to turn Wi-Fi off. File Transfer and Calibre Connect take the radio over when opened; OPDS, firmware update, font download, clock sync and KOReader login keep the remote reachable. The toggle is saved, so the remote reconnects in the background after every wake and restart; turning it on joins the saved network without opening the Wi-Fi screen.
- The top status bar shows a Wi-Fi symbol left of the battery percentage while Wi-Fi is connected; the screen repaints once when the connection comes or goes, or when the battery percentage changes (not in the reader).
- Debug builds (X4 Pro): `POST /api/cmd` runs the serial remote commands (keys, touch, typing, `KBDEXP`, settings) over Wi-Fi, from Goodies > Wi-Fi remote or File Transfer. Off unless `/debug/remote-token` is on the SD card; requests must carry that token. Usage: `docs/serial-remote.md`.
- Debug builds (X4 Pro): `POST /api/ota` flashes a firmware image over the Wi-Fi remote or File Transfer with no confirm step: token-gated like `/api/cmd`, streamed straight into the update slot, verified before it is selected, then the device restarts. The frontlight pulses while it streams, as in a file transfer. Usage: `docs/serial-remote.md`.
- Debug builds (X4 Pro): the remote can open screens (`GOTO <screen>`, `GOTO list`: Home, Library, Settings, Wi-Fi networks, Goodies, File Transfer, Calibre, OPDS, Nearby, resume reading and more) grab the screen over Wi-Fi (`/api/screenshot`, a PBM image) and tail the log live over Wi-Fi (`/api/psram-log?since=`). Usage: `docs/serial-remote.md`.
- OPDS: when a book finishes downloading, a prompt asks whether to open it now.
- OPDS: the search button also appears on catalogs that link their search through an OpenSearch description (such as a server's main page); the description is fetched on the first search and then cached.
- OPDS: book rows already in the download folder read "Downloaded". Read-only: one scan of the download folder per page.
- OPDS: the download screen shows the size received so far and the total ("12.3 / 33.0 MB").
- OPDS: when a book download fails, a prompt offers Retry or Cancel (back to the list, deleting the partial file). Retry continues from where the download stopped when the server supports resuming (HTTP Range, checked with If-Range), and otherwise starts over. A full SD card still shows its own error.
- OPDS: Back, Previous page and a prefetched Next page open straight from memory without a Loading screen first, and Back returns to the row you opened, scrolled as you left it.
- OPDS (X4 Pro): a catalog page shown from memory is fetched again in the background right away; if the server's copy changed (ignoring feed timestamps), the list redraws in place, keeping your row and scroll position.
- OPDS: catalog pages reuse one open connection to the server instead of a new secure handshake per page.
- OPDS: a book download that stops arriving is noticed after 10 s instead of 60 s and continues on its own from where it stopped (up to 4 times, while each attempt makes progress) before the Retry prompt appears. Large books from servers that cut long transfers now finish without a tap.

- The web status API (`/api/status`) reports the device's eFuse security state (flash encryption, secure boot, JTAG, USB-Serial-JTAG, download mode) and whether it is locked against reflashing. Debug builds log the same line at boot and in the PSRAM log header.
- USB Drive (X4 Pro): the frontlight pulses while the computer reads or writes the card, like Wi-Fi file transfer, and your brightness returns when you leave.
- Touch devices: Calibre Connect, alert screens and the full-screen dictionary get the top-left Back button used by File Transfer.
- Debug builds: the boot log names the exact panel controller (UC8179, SSD1677, ...), how it was detected, and the panel's VER/MTP product id and LUT version. Every PSRAM log grab (/api/psram-log, CMD:PSRAMLOG) starts with a header: device, serial, build, env, panel, uptime and heap.

- While File Transfer or Calibre Connect is moving data, the frontlight pulses between off and 25% once a second, and stays off while idle. Each pulse finishes smoothly, so even a short request gives one full blink and the light never cuts off abruptly. Your previous brightness returns when you leave. Changing the brightness yourself stops the pulse.
- Turn reading stats tracking on or off for the whole device or individual EPUB and XTC books, while keeping saved history and Time Left estimates.
- Assign separate short-press and long-press actions to the Left/Up and Right/Down side buttons; existing side-button layouts migrate to matching individual actions.
- Assign a side-button shortcut to flip the reading screen 180°, alongside clockwise and counterclockwise turns.
- Assign Library to power, long-press, button-chord, Home-button, or Quick Actions shortcuts to open the book list directly.
- Customize the top and bottom reader status bars separately, including item positions and progress bars, in EPUB, TXT, and XTC books. Each bar can be previewed where it appears while reading.
- View a selected book's reading stats from its Library or File Browser action menu.
- Library replaces Recent Books with a searchable book list, and adds various book metadata sort options.
- Reset a book's reader settings from the in-reader Settings tab.
- The OPDS browser asks before downloading a book that is already on the SD card, showing the existing file's size and date. Cancel is selected by default.
- Assign actions to upward and downward slides along either screen edge on touch devices.
- Automatic light sleep is available on the X3/X4 and Sticky and in the X4 Pro light-sleep firmware profiles, reducing idle power while retaining normal button, touch, and frontlight behavior. USB serial stays available while a computer is connected, because the device skips light sleep for as long as a USB host is attached. USB Drive also works in these profiles and keeps the device awake while it is open.
- Add the continuous **IncreMENTAL** EPUB indexing method for background chapter indexing.
- A new **Extend** sleep screen cover mode fills the empty margins around a cover that doesn't match the screen's aspect ratio by repeating its edge pixels instead of leaving them blank.
- A new **Extend Mirror** sleep screen cover mode fills those same margins by reflecting the cover's edge content instead of repeating a single edge pixel.
- TTF font support on ESP32-S3 devices. Whole-point sizes from 8pt to 22pt will be automatically available.
- The OPDS browser shows how many books a category holds, in parentheses next to its arrow, when the catalog provides a count (a `thr:count` link attribute or a "12713 books" summary). Folder titles that start with a 📁 emoji show as "/name" instead, since the device fonts have no folder emoji.
- On devices with PSRAM (Sticky, X4 Pro), the OPDS browser downloads the next page of a catalog in the background while you browse, and keeps pages you have visited in memory, so Next page, Previous page and Back open without waiting on the server.
- Sort the web file manager by name, size or modified date by clicking the column header. Click again to reverse the order. Folders always stay at the top, and the chosen order is kept while you browse other folders in the same tab.
- The web file manager's image preview has previous and next buttons, and the left and right arrow keys, to step through the images in the current folder. Other file types are skipped, and stepping wraps around at either end. The preview also shows the image's position in the folder, its pixel dimensions, its file size and, when the file list has one, its modified date.
- The Settings > System footer shows the firmware's branch (the batch number, such as `b11`, for combined test builds) and commit, with `*` when built from uncommitted changes, under the version. The System list stops above the footer instead of running under it. The web status API reports the full branch, build number (the commit count unless the build sets `CROSSDINK_BUILD_NUMBER`; left out for shallow checkouts) and UTC build time.
- In-reader menu for X3/X4/X4 Classic have been updated to a modified version of the in-reader menu for touch devices
- Chapter pages and book progress information is displayed in the frontlight drawer when in the reader for X4 Pro
- Add a Cover Grid Home theme on devices with PSRAM, showing the current book and six library covers.

### Changed
- Touch devices: lists open with no row highlighted. The first Up, Down or Confirm press shows the highlight without moving or opening anything; a row tapped to open a popup stays highlighted while the popup is up. The Home screen keeps its highlight.
- Settings: the selected tab is marked by a bar under its name instead of a black (inverted) highlight, and tapping a tab no longer highlights it; the box around the tab bar stays. Up/Down now wrap within the list and no longer move onto the tabs; long-press Up/Down still switches tabs.
- Touch devices: lists open with no row highlighted. The first Up, Down or Confirm press shows the highlight without moving or opening anything; a row tapped to open a popup stays highlighted while the popup is up. The Home screen and the chapter list (current chapter) keep their highlight.
- X4 Pro: with a USB host attached that is not reading the serial port, log lines skip the serial output instead of waiting up to 1 ms each (about 23 ms per page turn). The PSRAM and RTC logs still keep every line.
- Time to Sleep steps are now 1, 2, 3, 4, 5, 10, 20, 30 min, 1, 2, 4, 6, 8, 10 and 12 h, and Never, still on a slider (the web settings page shows them as a list). A saved time moves to the nearest step (a tie takes the shorter one).
- Crash reports are now saved to `/debug/crash_report.txt` and the battery diagnostic log to `/debug/battery_log.csv`, next to the other debug files on the SD card. An existing `/crash_report.txt` or `/battery_log.csv` is left where it is.
- Selection and tap highlights everywhere (list rows, Home menu tiles and cover cards, settings tabs, popups, reader drawer, buttons, keyboard keys and number fields) are marked with a thin outline instead of a dotted gray fill, which leaves less ghosting on the screen when the selection moves.
- Turbo keyboard (X4 Pro): about 27 ms less per key when typing quickly; the panel keeps its own copy of the last frame, so the firmware no longer re-sends it after each key.
- Wi-Fi: after a web request the device drops back to low power 0.5 s later instead of 2 s, and log tail and status polls (`/api/psram-log`, `/api/status`) drop it as soon as they are answered, so a log watcher no longer keeps the device awake. Uploads still run at full power until they finish.
- X4 Pro: less flashing. Leaving a gray page, or opening a menu over it, repaints only the gray pixels and what changed instead of the whole screen.
- X4 Pro: opening a book, a new chapter's first page, and returning to Home after Wi-Fi, OPDS, font download or update screens use a plain fast refresh instead of a full flash (about 0.9 s faster each).
- X4 Pro: pages with gray (anti-aliased text, gray images) now use the full balanced gray waveform instead of the stock quick gray pass, which pushed the panel one way on every gray page. Each gray page now flashes once and takes about 1 s longer.
- Turbo keyboard (X4 Pro): typing redraws only the letters that change, with a charge-balanced quick refresh (no one-way drive, pixels that stay the same are not driven), and tapped keys are no longer highlighted. The screen below redraws with the normal refresh when the keyboard closes.
- X4 Pro: the cleanup refresh (Half) no longer re-drives every white pixel black-to-white each time; it runs only the real changes from the previous screen.
- Keyboard (X4 Pro): keys tapped while the screen is still updating now all appear together in the next update instead of one update per key.
- X4 Pro: leaving a grayscale sleep image uses a charge-balanced drive for the first black-and-white screen.
- OPDS (X4 Pro, Sticky): up to 64 catalog pages stay in memory for Back and Previous (was 24), and background page preloads no longer use the internal RAM that reading and leaving Wi-Fi without a restart need.
- Turbo keyboard (X4 Pro): the screen's previous-frame memory is always refreshed after each key, so later cleanup refreshes never re-drive settled pixels.
- Turbo keyboard (X4 Pro): each key sends the whole screen to the panel instead of only the text and key areas, so the panel's image memory always matches the screen.
- X4 Pro: the reader's automatic cleanups no longer flash (Refresh Screen still does a full refresh). Opening a book, returning to it, the page after an image, and the regular ghost cleanup use a ~0.3 s no-flash scrub instead of the 1.5 s flashing refresh, and closing a menu over an image page skips the cleanup. Image pages also finish their anti-aliasing sooner, and a menu opened during an image cleanup shows ~0.6 s sooner.
- X4 Pro: closing the Turbo keyboard cleans the screen with the same no-flash scrub.
- X4 Pro: an OPDS book download switches the panel's power booster off after each progress update, like the transfer screens; interactive screens keep it on so input stays fast.
- X4 Pro: the panel temperature check no longer runs right after a refresh, and a cold panel (below 15 C) uses the matching full-refresh waveform.
- Opening an EPUB for the first time shows only the Indexing popup, not Loading first, so indexing starts ~0.6 s sooner.
- Joining a saved Wi-Fi network shows the Connecting screen only if the join takes longer than 0.7 s.
- The OPDS download screen no longer refreshes twice when a download starts.
- X4 Pro: the SD card and online firmware update, Calibre Connect, Nearby transfer and font download progress screens clear the previous screen with a longer 15-frame scrub (no flash) each time they appear. OPDS uses the same longer scrub. The Turbo keyboard opens with this scrub instead of the flashing refresh.
- OPDS book downloads and File Transfer uploads run their network work on the second CPU core and write to the SD card in the background, so transfers are faster.
- The OPDS catalog and the KOReader Sync result screen let the device doze while you read the list, instead of keeping the CPU at full speed with Wi-Fi up.
- KOReader Sync retries a request once when the connection fails, and uploads start sooner.
- X4 Pro: after a restart (network screens, firmware update), the SD card mounts about 0.2 s sooner.
- X4 Pro: flashing a firmware sent from File Transfer opens the update screen without the full-screen flash and one redraw sooner.
- File Transfer and Calibre Connect use less power while idle: the device wakes about 4 times a second instead of 20.
- Debug builds: `[PM]` lines count main-loop passes (`loop=N`), a quiet window with a busy core names the tasks behind it, worker tasks log their lowest stack headroom when they exit, and the boot timing line splits panel detection from panel start.
- OPDS: every screen (loading, downloading, errors) uses the same header with the status bar; on touch its arrow goes back, or cancels a download. After a download the book list is drawn first, then the open prompt appears over it.
- X4 Pro: the first File Transfer QR frame runs a longer scrub (no flash), so the Wi-Fi list or keyboard no longer shows through the QR. The first OPDS frame and the first download frame run a quick scrub that clears the previous screen's ghost.
- File Transfer, Calibre and USB Drive keep a lit frontlight at your level for 10 seconds before the transfer pulse takes over.
- File Transfer, Calibre, USB Drive and SD firmware update: the transfer pulse starts and ends at your brightness and rests there when idle, so it always fades smoothly back to your level. It pulses 0-10% at brightness up to 10% (or with the light off), 10-25% at 11-25%, and between 10% and your brightness above 25%.
- OPDS downloads write to the SD card in 32 KB blocks (PSRAM buffer) instead of one write per network packet.
- File Transfer and Calibre Connect: the frontlight pulse starts once the server is running, so the mode menu and Wi-Fi picker keep your brightness.
- KOReader sync reuses one TLS connection for the progress download, the second document-id check and the upload, instead of a new handshake for each. TLS handshakes (sync, OPDS, downloads) use faster elliptic-curve math.
- KOReader sync is faster: it skips the time sync when the clock is already set, shows one status screen instead of three, no longer waits for each screen to finish drawing before the request, and gives up on an unresponsive server after 8 seconds. Smart sync no longer tries the second document id after a network or login error.
- Debug builds: tap and swipe logs use screen coordinates in the current orientation and name the swipe direction.
- X4 Pro: the keyboard refresh no longer reads `kbd-exp.txt` from the SD card; the debug serial `CMD:KBDEXP` override is kept in RAM until reboot.
- File Transfer, Calibre Connect, Nearby transfer and USB Drive switch the panel's power booster off right after each screen update instead of after 8 seconds idle, saving power while they wait.
- USB Drive logs how long the computer took to mount the card (connect, first read, end of the initial scan).
- Reader: opening the menu, quick actions or the frontlight panel, rotating, jumping, skipping chapters, sleeping, locking, pressing Back and similar actions now stop a running anti-aliasing pass instead of waiting for it to finish. The page gets its anti-aliasing again when you come back to it.
- X4 Pro: the standard `x4-pro` firmware now uses automatic light sleep (it was the separate `x4-pro-light-sleep` build), and `x4-pro-debug` is its debug build with the timing and PSRAM log tools. The `x4-pro-light-sleep` and `x4-pro-light-sleep-debug` environments are removed.
- Debug builds: the `[PM]` power line now also ends when the screen changes and names it (`act=`), and counts button, touch and timer wakes. The `[CPU]` line names the current screen too, so power and load can be grouped per screen.
- Home no longer shows a Loading popup the first time you return from a book. While you read, the book's Home cover thumbnails are made in the background on the second core once the page has been still for a few seconds. Any cover still missing when Home opens is made in the background while Home is already drawn and usable, and it appears when ready. XTC books now pre-make the right thumbnail sizes for every Home theme (the Carousel sizes were wrong before).
- X4 Pro: Turbo keyboard (Settings → System → Device) is on by default for new settings. A saved setting keeps its value.
- X4 Pro: Turbo keyboard now drives 6 frames per key (was 3), matching the tested setup; 3 frames left heavy ghosting.
- Touch keyboard: every tap inside the keyboard now types a key. Gaps between keys are split between neighbours, the outer keys reach the screen edges, and the top row reaches halfway up the strip above it, so there are no dead spots. The keys look the same.

- On touch devices the keyboard no longer highlights a key when it opens, and the Up and Down buttons move the text cursor left and right. A side button press brings the highlight back for button typing.
- Updating firmware from the SD card is faster: picking a file checks only its header, the full checksum and SHA-256 are verified while it is written (the new firmware is only activated when they match), the next part of the file is read while the current part is written, blank flash is not erased or written again, and the progress bar moves in 5% steps. The frontlight pulses while it flashes and stays on until the restart, and the touchscreen sleeps meanwhile.
- X4 Pro light-sleep profiles: Wi-Fi rejoins a saved network with a fast scan on its known channel, and uploads use larger TCP windows, 12 KB WebSocket chunks and a background SD writer, so they reach the device faster.
- Updating firmware from the SD card checks the image's SHA-256 while writing it, instead of reading the whole file twice, and shows its progress with the fast keyboard refresh.
- After a silent restart the screen keeps its previous frame instead of doing a full refresh.
- Large images in books that were re-decoded on every page now stay cached on PSRAM devices.
- Buttons and touch are read on their own task, woken by the input lines, so presses, taps and swipes made while the device is busy drawing or indexing are queued instead of lost.
- A chapter's first open inflates it on the second core while the screen task parses and lays it out, instead of unpacking it to the SD card first.
- JPEG and PNG images in books decode on the second core while the screen task dithers and draws the rows already decoded.
- Opening File Transfer, OPDS, KOReader Sync, update check or Manage Fonts no longer reboots the device when enough memory is free; reader caches are released in place instead.
- Leaving File Transfer, Calibre Connect, OPDS, KOReader Sync or Nearby Transfer no longer reboots the device when enough memory is left; Wi-Fi is shut down in place instead.
- Larger allocations, Wi-Fi buffers and some debug buffers now use PSRAM by default, leaving more internal RAM free.
- The next page is drawn ahead on the second core right after a page is shown, instead of waiting for the reader to sit idle. Pages using SD card fonts or with saved clippings keep the idle draw-ahead.
- The next chapter is indexed on the second core near the end of a chapter, so page turns are not held up by it. Books using SD card fonts still index it on the screen task.
- File Transfer and Calibre Connect serve requests on their own task on the second core, so transfers keep flowing while the screen redraws.
- TTF fonts opened from memory get a second FreeType instance for background work on the second core, so later background layout and drawing do not wait for the screen's font lock. Fonts streamed from the SD card still use the screen's instance.
- Every remaining CrossInk name in code, build flags, scripts, and docs is now CrossDink. On-device data paths and file formats keep their names, so existing settings, stats backups, and optimized books keep working.
- File Transfer and Calibre on a Wi-Fi network now idle in modem sleep and light sleep between transfers, and switch to full power from the first byte of a request or upload until two seconds after the last one. Hotspot mode keeps the radio fully on.
- The firmware is renamed CrossDink, with a new two-drop logo on the boot screen and web portal. Existing settings, caches, and device paths are unchanged.
- USB Drive mounts faster: the next part of the SD card is read in the background while the current data is sent over USB, cutting about 5 seconds from mounting a FAT32 card on the X4 Pro.
- Set Power short-press and long-press to Sleep, Wake, or Sleep/Wake separately; holding Power can always wake the device. Chord shortcuts and the home button can also now sleep the device.
- Brightness and warmth gestures now respond while you drag, with longer swipes making larger adjustments.
- Edge-slide and two-finger brightness and warmth gestures now adjust in 1% steps instead of 5% and are half as sensitive: a full-length slide changes the level by about 50%.
- Reversing a brightness or warmth drag partway now moves the level past where it started instead of stopping there.
- Idle power saving now puts the device into automatic light sleep between loop ticks instead of only lowering the CPU clock, cutting idle draw while leaving buttons, touch, and the frontlight working exactly as before. The screen, Wi-Fi transfers, and USB sessions stay awake while they are in use.
- GitHub workflows and release documentation links now follow the `development` default branch.
- The built-in Bitter and Lexend Deca TTF fonts carry only the ligature and kerning data the reader uses, freeing about 290 KB of flash on ESP32-S3 builds. Text renders the same.
- The web portal pages ship minified JavaScript and CSS, freeing about 22 KB of flash and loading the Files page faster over Wi-Fi. Builds need Node (npx) for this; without it the pages are served unminified as before.
- Idle power saving now engages after 250 ms instead of 3 seconds, and battery level is polled every 6 seconds instead of every 1.5, trading slightly less frequent battery updates for lower average power draw.
- The Library opens instantly when nothing on the SD card has changed since its last scan, instead of rescanning the whole card on every visit. Moving the selection no longer re-reads each visible book from the card.
- After a restart, Home starts indexing the Library in the background as soon as it appears, so the first Library visit usually opens without the "Reading your books" wait. Indexing pauses the moment you press a button or touch the screen and picks up where it left off.
- Library scans no longer re-read books whose title and author could not be read last time; they keep their filename until the file changes or you use the Library's refresh, which retries them.
- Fewer SD card writes: session state and reading stats are no longer rewritten when nothing changed, and the reading percentage shown on Home is saved once when you leave a book instead of every 10 pages.
- Far fewer SD card writes when saving your place: EPUB progress now alternates between two small slot files that are overwritten in place, so a save costs about 2 sector writes instead of roughly a dozen, and a save that would store the position already on the card (such as closing a book without turning a page) writes nothing. Your place is now saved every 30 page turns or 15 minutes of reading instead of every 10 pages or 5 minutes; leaving the book or putting the device to sleep still saves it immediately, so only a crash, reset or dead battery can lose more pages than before. A save interrupted by power loss falls back to the previous save. TXT and XTC progress and the Home reading percentage are also overwritten in place instead of being truncated and rewritten.
- Firmware is about 32 KB smaller: wolfSSL no longer builds its debug trace messages in. Builds with `-DFREEINK_WOLFSSL_DEBUG` still include them.
- The X4 Pro light-sleep firmware now uses the scalable TTF versions of Bitter and Lexend Deca, like the standard X4 Pro build, making it about 360 KB smaller.
- Text drawing resolves clipping and screen rotation once per glyph, reducing work when painting menus and book pages.
- Library reuses its index on return visits and refreshes after file changes, instead of scanning the card every time.
- Home reads saved EPUB progress and chapter metadata without opening or indexing the book, and stops saved-item checks after the first file.
- Optional EPUB background work yields immediately when rendering is busy, keeping input polling responsive.
- SD-card fonts share identical character lookup tables across styles, reducing memory use and repeated card reads.
- EPUB reader menus now share five tabs across devices. Button devices gain live font and margin previews, Reading Stats, and in-book transfer options.
- The on-screen keyboard now uses wider outlined keys with clearer spacing on touch and button devices.
- Long status titles shorten faster when they do not fit the screen.
- Leaving an EPUB or TXT reader releases rebuildable font buffers for other screens.

### Fixed
- Tapping a list row that opens a popup now shows the row selected before the popup appears, instead of only after it closes.
- Web portal and WebDAV: `/debug/remote-token` (the Wi-Fi remote token) can no longer be downloaded, replaced, renamed or deleted over the network, under any spelling of its name, even with Show Hidden Files on; the `/debug` folder itself can no longer be renamed, moved or copied there, which would carry the token out with it.
- X4 Pro: the transfer light pulse (File Transfer, Calibre, USB Drive, firmware updates) ramps smoothly again instead of stepping, and File Transfer no longer pulses when the only traffic is log or status polling.
- A two-finger or edge brightness slide that starts with the light off and ends at or below where it started now leaves the light off.
- A brightness slide during a transfer light pulse now starts from your own brightness instead of the pulse level, and the level you slide to is kept when the transfer ends.
- X4 Pro: a brightness slide while Dim Light on Flash has the light ducked no longer jumps it to full mid-flash; it fades back up to the slid level when the flash ends.
- X4 Pro USB Drive: the frontlight pulse starts only once the computer moves at least 16 KB within 0.1 s, so idle polling no longer blinks it, and keeps going through pauses of up to 2 s between bursts of a copy instead of dropping to its floor.
- X4 Pro: with Light Timeout on, a transfer pulse on an idle device (such as an update over the Wi-Fi remote) no longer flickers: the timeout counts from the end of the pulse, then fades as usual.
- X4 Pro: the transfer light pulse (File Transfer, Calibre, USB Drive, firmware updates) ramps smoothly again instead of stepping, and File Transfer no longer pulses when the only traffic is log or status polling.
- X4 Pro: every reboot (update, restart into Wi-Fi or the reader, remote reboot) now switches the screen's power off first, instead of leaving it powered until the reset.
- X4 Pro (Softfast): black and white pixels that stay the same across page turns (the status bar, overlapping text, the background) get a short balanced re-drive at the end of each turn, so they no longer fade or get dirty; a turn right after a skipped gray pass draws its text as dark as a regular turn; register waveforms run at the panel's own voltages and VCOM; balanced repaints run longer on a cold panel.
- X4 Pro (Softfast): black pixels that stay the same across page turns (the status bar, overlapping text) get a short balanced re-drive at the end of each turn, so they no longer fade; a turn right after a skipped gray pass draws its text as dark as a regular turn; register waveforms run at the panel's own voltages and VCOM; balanced repaints run longer on a cold panel.
- X4 Pro (Sharpflash, and Softfast's first page after opening a book or closing a menu): the anti-aliased page draws in one flash, instead of showing black-and-white text, flashing, then the gray. It also appears sooner.
- OPDS: loading catalog pages and downloads uses 2 KB less of the main task's stack (the network read buffer moved off the stack).
- Logs: download URLs are logged without their query string, so signed download tokens stay out of logs, and a line cut at the length limit still ends with a newline instead of running into the next one.
- X4 Pro (Softfast): the reader's top and bottom panels and other menus opened over an anti-aliased page draw their black text with a longer balanced repaint (about 0.5 s more when opening them).
- X4 Pro: Softfast no longer lets the page get dirtier over time: every Refresh Frequency pages it runs one full gray refresh (one flash) that redraws every pixel.
- X4 Pro: menus and the reader drawer opened over an anti-aliased page no longer show their text slightly gray (and no longer darken step by step on repeated taps). Leaving a gray page takes about 0.5 s longer.
- X4 Pro: the OPDS download page, Calibre upload progress and Nearby send/receive progress keep the screen powered while the progress updates. Switching the screen off between updates left heavy ghosting on a large OPDS download.
- OPDS: Back (button or the top-left Back button) now cancels a catalog page that is still loading and returns to the previous list, instead of being ignored until the request finishes or times out after 60 s. A kept-alive connection idle more than 4 s is reopened instead of reused, so a page opened after a pause no longer hangs on a socket the server dropped.
- Touch: tapping a row in Goodies (such as Wi-Fi remote), Library settings, or Settings opened from the file browser now leaves that row highlighted, as tapping does on the other settings lists. Goodies no longer jumps the highlight back to the top row after the Wi-Fi remote toggle.
- Debug builds (X4 Pro): turning Goodies > Wi-Fi remote off no longer holds up the tap for about 120 ms; Wi-Fi shuts down in the background.
- Debug builds: after a task-watchdog reset, the SD crash report names the task each core was running.
- X4 Pro: after a crash during start-up, a later restart no longer uses an out-of-date copy of the screen as its starting point, which could re-drive pixels that were already set.
- With the Cover Grid Home, a large or imperfect library (for example more than 1024 books in one folder chain) no longer shows the "Scanning library" popup and rescans the card every time Home opens. A best-effort index is kept until the card changes; the Library still repairs it on its own visit.
- X4 Pro: with anti-aliasing on, reading no longer adds a periodic cleanup flash; each anti-aliased page already redraws every pixel. The manual Refresh Screen shortcut still works.
- X4 Pro: turning from one anti-aliased page to another flashes once instead of twice.
- X4 Pro: turning past a page with an image no longer adds a full-screen flash on the next page. The panel already repaints every pixel cleanly when it leaves the image's gray.
- X3/X4 (SSD1677): anti-aliased text no longer pushes gray pixels one way on every AA page. The AA gray pass now drives each gray level both ways (about 21 frames instead of 12, no extra flash), and the firmware refuses to build if any SSD1677 waveform is DC-unbalanced.
- X4 Pro: the display no longer uses one-way (DC-unbalanced) drives, which built up charge and made screens dirtier over time and risked lasting image retention. Every refresh now uses the panel's own waveforms: screen entries, typing, File Transfer / OPDS / Calibre / Nearby / OTA / font download / SD firmware progress repaints are OTP Fast, and reader ghost cleanups are the balanced Half again (a short flash at your refresh-frequency setting). Typing is slower per key (about 0.55 s instead of 0.3 s ink), and headers on long transfer screens may fade slightly.
- OPDS (X4 Pro, Sticky): leaving the catalog after browsing no longer restarts the device. The secure-connection session kept for the next visit now lives in PSRAM instead of splitting the internal RAM block that returning without a restart needs.
- X4 Pro: the first time a page with a large image (such as a book's cover) opens, the image is unpacked and decoded in the background. The page shows its image frame at once and fills it in when ready; buttons, swipes and menus keep working meanwhile instead of freezing for several seconds.
- After a KOReader Sync or Nearby sync, later Wi-Fi sessions in the same boot no longer run with Wi-Fi power saving off.
- Using Wi-Fi once no longer leaves internal memory fragmented until the next reboot.
- X4 Pro: the sleep screen no longer ghosts when the device falls asleep on its own after sitting idle. Every sleep screen now starts from a freshly powered panel with the keyboard fast waveform switched off, the same as a power-button sleep.
- KOReader sync: pressing power or letting the device sleep during a slow sync no longer freezes it for up to ~16 seconds; it waits at most 3 seconds, then sleeps. The sync worker has a larger stack for the faster TLS math.
- Debug builds: serial file downloads and CMD:PSRAMLOG dumps no longer lose bytes when the USB buffer is full, and a log line can no longer land inside a serial reply or binary stream. A reply to a host that has closed the port no longer stalls the device for half a second. Blank lines and xink-remote's ">>>>> " echo lines are ignored quietly.
- Reader: a page whose anti-aliasing pass was cancelled by a page turn that was then dropped (for example "previous" on the first page) no longer stays without anti-aliasing until the next turn.
- Background cover and image workers can no longer borrow the screen buffer while a book page is being built, so pages and cached covers are no longer corrupted when both happen at once.
- Drawing the next reader page ahead of time no longer blanks the page on screen or leaves old text under the next page.
- Check for update now looks at CrossDink's own GitHub releases instead of upstream CrossInk's, so an upstream release can no longer be offered and flashed over CrossDink. It only accepts a release firmware for this device (`firmware-<device>-v<version>.bin`).
- X4 Pro: touch, the battery gauge and the clock share one I2C bus, and a battery or clock read from another task could overwrite a touch reading mid-copy (phantom taps, odd battery or temperature values). Each I2C transaction now holds the bus until its data is copied out, and the clock's cached time can no longer be read half-updated.
- X4 Pro: the header and other still text no longer fade on screens that repaint progress over and over (file transfer, Calibre, Nearby transfer, OPDS and font downloads, OTA update). Those repaints now use the keyboard's fast waveform, which re-darkens unchanged black pixels.
- OPDS: book downloads run in the background, so Back and the Cancel button stop them at any point (the partial file is deleted) and the screen shows Connecting until the first byte arrives. Downloads also start sooner: the free-space check no longer scans the whole SD card first.
- Keyboard: kbd-exp.txt is only read by debug builds.
- X4 Pro: joining Wi-Fi is about 1.5 s faster. The address conflict check that was meant to be off was still running on every join.
- X4 Pro keyboard: keys you have not pressed no longer fade during long typing sessions; every keystroke now also re-darkens black pixels that stay black.

- A chapter left half-indexed by an older firmware is re-indexed instead of resuming with outdated page positions.
- Saving the reading position before opening Wi-Fi, Calibre, KOReader sign-in, file transfer or the light panel waits for the screen to finish drawing, so it can no longer read or close a chapter that is still being indexed.
- Home no longer rebuilds the Library index over and over in the background when the result is degraded (for example more than 1024 books in one folder chain). That loop rewrote the index on the SD card on every pass and kept the device from auto-sleeping on Home. A degraded background index is now left for the Library to rebuild, and is retried only after the card changes. An index whose "Recent" (date added) order fell back is also left for the Library instead of being used as current.
- File Transfer choices no longer appear preselected when opened on a touch device.
- On the X4 Pro and Sticky, the Home screen no longer stalls waiting for the SD card while the Library indexes in the background.
- Small images that are scaled up, such as a short progressive JPEG, are now cached after the first draw instead of being decoded again on every screen refresh.
- The serial log no longer reports errors for normal events: a missing image, section or dictionary cache file on first open, an outdated section cache being rebuilt, and a device without a clock chip or motion sensor. Log lines that were missing a line break now end cleanly, and SD, display, TLS and frontlight messages use the standard log format.
- X4 Pro light-sleep firmware shows its real version (for example `1.6.0-x4-pro`) instead of "dev" in Settings, on the boot screen, and in the web and OTA version checks.
- Background Library indexing and the reader's next-page draw-ahead run at full CPU speed again instead of the lowest idle clock.
- USB Drive no longer reads ahead into the sectors a computer is about to write, so copying files to the card is not slowed by background reads.
- Nearby and KOReader position sync read the chapter layout of the orientation the book is read in, instead of whichever orientation the sync screen happened to use, so a book read in landscape lands on the right page.
- Your reading position is saved before a reader shortcut starts Calibre Wireless, Join Network or Create Hotspot, and before the light panel or KOReader sign-in can lead to a restart, instead of reopening the book up to 29 pages behind.
- On the X4, X3 and Sticky, consecutive EPUB image pages no longer add an extra full-screen flash; the image-to-image cleanup refresh now runs only on the X4 Pro panel that needs it.
- EPUB anti-aliased page turns on the X3/X4 no longer free and reallocate an 8 KB render buffer on every page, which could fragment memory and fall back to slower rendering.
- Changing line spacing or other layout settings from the reader menu can no longer show a page drawn ahead with the old layout on PSRAM devices.
- Pressing Confirm on a Quick Actions "Next page" at the end of a book no longer opens the first suggested book or the reader menu.
- Extend and Extend Mirror sleep covers fill both margins when the cover image is larger than the screen and is scaled while drawing.
- The USB serial connection stays available under automatic light sleep on every build that uses it, not only the X4 Pro light-sleep profiles.
- Retrying an OPDS folder that showed "No entries" now asks the server again instead of showing the cached empty page on PSRAM devices, and indented "12713 books" summaries show their count.
- A re-downloaded OPDS book that cannot be moved into place keeps the previous copy instead of deleting both.
- A background Library index that could not be sorted or de-duplicated for lack of memory is rebuilt by the Library instead of being kept as current.
- The web file manager ignores the Enter that commits an IME composition or repeats while held, so rename and move are not sent twice or half-typed.

- In the OPDS browser, Back (the header button, the back swipe or the Back button) now goes up to the catalog you came from instead of stepping back through each Next/Previous page you visited.
- Chapters left partly indexed by v1.6.0 now re-index after updating instead of resuming with pages laid out under the old rules.
- Edge slides, two-finger swipes and header taps work reliably on the File Transfer screen while the web server is running.
- Dragging or scrolling on a list or menu no longer highlights or selects the row under your finger, and a short drag no longer opens it. Rows highlight once your finger rests on them briefly, so long-press still works.
- Turning from an EPUB image page whose grayscale pass finished to another image page now runs a cleanup refresh, so the previous image no longer ghosts on the X4 Pro. Image pages skipped before their grayscale pass finishes no longer force a flash on the next page.
- Keyboard rows are shorter on button-only devices so side-button hints no longer cover the keys.
- File Transfer choices no longer appear preselected when opened on a touch device.
- Saved clipping lists now show a scrollbar when more clippings are available below the visible rows.
- EPUB Safe Mode no longer pins inherited fonts and page layout as personal book settings.
- The X4 Pro Home button now steps back through dictionary lookup, chapter selection, and nested settings instead of jumping to Home.
- OPDS downloads now use the first listed author for filename templates when a catalog also lists translators or other contributors.
- Large OPDS downloads no longer fail partway on the X3/X4. TLS keeps one receive buffer per connection instead of allocating 17 KB for every record, and asks servers for smaller records.
- An interrupted or cancelled OPDS download no longer leaves a broken book in the library. Books download to a `.part` file that is renamed only when complete, and an EPUB cut short by a server that sends no length is rejected.
- OPDS downloads check SD card free space first and show "Insufficient SD card space" instead of failing midway.
- The Cancel button on the OPDS download screen responds to a tap during the download.
- Retain the CSS spacing supplied by empty inline spans.
- X3/X4 firmware builds again with automatic light sleep enabled. The build platform is updated to pioarduino 55.03.39 (Arduino-ESP32 3.3.9, ESP-IDF 5.5.4), which fixes a linker-script mismatch that stopped the image from being created.
- X4 Pro firmware with automatic light sleep now enters deep sleep correctly when the frontlight is enabled.
- Periodic memory telemetry now reports only when free heap or PSRAM changes, while checking for changes every 2 seconds.
- Incremental EPUB indexing resumes after skipping chapters and refreshes the status bar when indexing completes.
- Rapid queued EPUB page turns skip rendering intermediate pages until the final destination.
- Automatic light sleep can no longer engage while the e-ink panel is mid-refresh, closing a latent waveform/SPI timing risk on devices with idle power saving enabled.
- Improve stability when connecting to Wi-Fi for update checks and KOReader authentication on X4 Pro.
- Crash reports now identify the primary CPU core, show task names when available, preserve both cores' backtraces, and include the firmware ELF hash needed to decode them.
- Release clipping index memory after closing a book or clearing its clippings.
- Keep clipped text, exported excerpts, and chapter titles on complete characters when shortened.
- Keep clipping-selection button hints from covering book text.
- Changing a reader font with incremental indexing now returns after the current reading position is ready, instead of waiting for the whole chapter to be re-indexed.
- On the first page of an EPUB or XTC book, previous page and previous chapter (including the chapter-skip long press) now do nothing instead of redrawing or reloading the page.
- A button press made while the end-of-book "Continue with" menu is still appearing is no longer lost: it moves the selection, opens the book, or goes back once the menu is ready.
- The image viewer redraws the image after you close the pull-down top panel, the image action menu, or a prompt, instead of leaving the panel or menu on screen.
- 8-bit and other paletted BMPs saved with a newer (V4/V5) header, as GIMP and ImageMagick write them, now show their real gray levels. White backgrounds no longer turn into dither dots and black no longer shows as dark gray, in the image viewer and on sleep and boot screens.
- Release builds use the pinned PlatformIO core during nested ESP-IDF configuration.
- Adding the sleep moon to the last screen no longer flashes white in night mode.
- Waking the reader skips the intermediate loading icon refresh.
- Screenshot folder names keep complete non-English characters when shortened.
- Longer power-on instructions wrap on the finished update screen.
- Sticky now records periodic heap and PSRAM statistics over its ROM logging path.
- RTL EPUBs use reading-order swipe and tap directions.
- Korean text keeps natural syllable spacing when justified and wraps by word.
- Footnote choices can be selected directly on the reading page, with a list fallback for links without a visible target.
- Changing global font or page layout settings from the pull-down panel on touch devices now updates the open book when it inherits those settings.
- KOReader authentication now rejects unexpectedly large server responses to avoid crashes.

## [v1.6.0] - 2026-09-21

### Added

- EPUBs with stable page numbers can jump directly to a specific stable page from the reader menu.
- Hidden folders can be created using the web file manager now when prefixed with a dot.
- Choose whole numbers, one decimal, or two decimals for the book progress percentage in status bar settings.
- Two-finger Screen Rotation can be turned off in Settings > Controls > Taps & Gestures on multi-touch devices.
- Go to % and Go to Stable Page use a numeric keypad for typing an exact destination, including decimal percentages. Touch devices use the keypad exclusively; button-only devices keep the slider by default and hold Confirm/Select to switch to the keypad.
- Files can be renamed from the File Browser action menu while keeping reading progress, bookmarks, clippings, and recent-book entries linked to the new name.
- Firmware builds can include only selected UI languages to reduce flash usage while preserving English fallback.

### Changed

- PNG, XTC, and image-dithering scratch buffers use fewer heap allocations to reduce fragmentation.
- The shared settings catalog keeps its initial allocation instead of retaining unused vector capacity.
- SPI SD-card transfers are batched through the ESP32 hardware FIFO for faster reads.
- SD-card font prewarming releases temporary lookup buffers before allocating large glyph bitmaps.
- UC8179 grayscale images use a slightly longer waveform for stronger midtone separation.
- EPUB image preparation writes extracted data in chunks and reuses two cached images on PSRAM readers.
- Font menus and the web portal use a persistent catalog that loads one family's details at a time, preventing crashes with larger font collections.
- Web portal pages reuse browser-cached content after checking for firmware updates.
- Rapid queued EPUB page turns defer text anti-aliasing and image loading until the final page, making intermediate turns faster.
- Grayscale sleep screen images use the panel's direct grayscale waveform where supported, which folds the base frame into the grayscale pass instead of refreshing the screen separately first.

### Fixed

- The web EPUB optimizer now accepts books that use standard Adobe or IDPF font obfuscation, while leaving DRM-protected books unchanged.
- Frontlight schedule time pickers now use the compact number keypad from Go To screens.
- X4 Classic's left/right tilt direction labels now match the physical page-turn direction.
- Touch keyboards no longer show button-only hold and navigation hints.
- The web settings page no longer offers the Up + Down shortcut on devices that cannot use it.
- OPDS Wi-Fi selection and search entry stay awake while the user is actively choosing or typing.
- USB Drive exits cleanly when a connected host is unplugged without ejecting first.
- EPUB ordered lists show numbers, respect marker-free styles, and retain their container indentation.
- EPUB chapter layout releases rebuildable font caches first, reducing low-memory failures on X3/X4.
- KOReader Sync uploads retain exact text-node positions, including zero offsets and UTF-8 text.
- Saved clipping highlights now retain Focus Reading's custom-font glyphs instead of showing replacement characters.
- EPUB dictionary lookup can select an individual part of a hyphenated word.
- Short Power-button frontlight and touchscreen shortcuts in EPUB books no longer run the configured long-press action.
- Silent restarts now preserve the frontlight state instead of applying wake or schedule settings.
- The Home button now returns from Status Bars to the previous menu instead of leaving the reader.
- OPDS book downloads can follow secure redirects without sharing catalog credentials with the download host.
- Larger EPUB stylesheets work on PSRAM readers, including rules that hide duplicate images.
- JPEG-heavy EPUBs can use PSRAM for decoding on supported readers, leaving internal memory available for reading.
- Importing CrossPoint settings preserves tap and swipe modes without carrying over a stale reader touchscreen lock.
- Saved clippings no longer highlight unrelated single words at page boundaries when matching text after a layout change.
- Quick Lock sleep now respects the configured short Power-button wake behavior.
- Quick Lock now clears when the device wakes after an automatic sleep timeout.
- EPUB content marked with the HTML hidden attribute no longer appears in the reader.
- EPUB paragraphs without source indentation no longer gain a synthetic first-line indent.
- End-of-book selection remains consistent during concurrent redraws.
- Image dithering reports low-memory failures instead of aborting during buffer allocation.
- The debugging monitor plots CrossDink heap and PSRAM logs separately; ZIP failures identify the affected EPUB entry.
- Many progressive JPEG images that store brightness and color in separate scans now render instead of appearing blank.
- PNG sleep overlays preserve four evenly spaced grayscale levels on supported displays.
- Exiting Calibre Wireless on X4 now returns Home with one clean screen refresh instead of repeated blank flashes.
- Manage Fonts no longer crashes after Wi-Fi connects on ESP32-S3 readers.
- Editing font settings from the top drawer's global settings within a book now applies those changes when no per-book font settings exist.
- Per-book reading stats now write to a backup file first.
- Paragraph-alignment previews remain available on text-heavy pages instead of disappearing when the preview sample is full.
- Quick Actions assignments stay visible in button-combo settings, and X4 Classic can use the Up + Down shortcut.
- Sync Progress from the reader menu opens KOReader setup when credentials have not been configured.
- Button-combo settings no longer offer Sleep because the same combo cannot wake the reader.
- EPUB variation selectors no longer appear as missing-glyph boxes after otherwise supported symbols.
- Cancelling Word Spacing on button readers no longer briefly changes the slider value.
- The File Browser now displays decomposed Hangul and accented filenames copied from macOS correctly.

## [v1.5.1] - 2026-09-10

### Added

- Xteink X4 Pro and X4 Classic support, including device-specific firmware and USB Drive access; X4 Pro also supports direct USB file transfers.
- The built-in EPUB optimizer can keep cover art in color while still resizing it to a reader-safe baseline JPEG.
- Custom BMP boot screens, selected in the File Browser or rotated from `/bootscreen` or `/.bootscreen`; sleep screens can also be selected from any folder.
- Quick Lock, assignable button combinations, and shortcuts for Previous Page and Nearby Position Sync. Quick Actions can also be assigned to Power + Up and X4 Pro Home-button gestures.
- Configurable touch page-turn gestures, pinch-to-resize text, two-finger rotation and swipe actions, and a tap-to-hide reader status bar.
- Selectable keyboard layouts, switchable from the keyboard's language key.
- Clippings from dictionary lookups on touch devices, plus selection of text inside EPUB tables.

### Changed

- Touch EPUB readers use a half-height, five-tab menu. Sticky opens the menu with a swipe up and book details with a swipe down; X4 Pro frontlight controls include reading stats and reader shortcuts.
- Screen margins have separate Top/Bottom and Left/Right controls, adjustable up to 200 pixels.
- Night Mode applies system-wide on ESP32-S3 devices; frontlit readers can disable periodic full-screen refreshes.
- Waking keeps the sleep screen visible until the reader or Home is ready, unless a custom boot screen is enabled.
- Font choices show available point sizes, Download Fonts replaces the font-manager label, and Wi-Fi passwords are visible during entry.
- Reader controls, shortcut pickers, touch targets, and File Browser settings are easier to reach; Book Options is last in the button reader menu.
- EPUB indexing, image decoding, fonts, and reading-state updates use fewer resources; the web optimizer prefers natural boundaries when splitting chapters.

### Removed

- The undocumented X4 Pro power-button double-click frontlight toggle.
- Built-in reader-font emoticons and hand gestures; SD-card fonts retain emoji fallback support.

### Fixed

- Clipping highlights stay aligned after font changes, retain multi-paragraph text, and remain readable in Dark Mode. Selection stays on its final page, and browsing saved clippings responds reliably.
- Dictionary lookup respects landscape controls and selected fonts, handles repeated lookups more reliably, and returns to the reader cleanly when dismissed.
- EPUB tables retain column widths and wrap long labels; mixed-direction text, Arabic/Persian shaping, ruby annotations, and footnote styling render correctly.
- EPUB contents links, split-chapter navigation, footnote resumes, and end-of-book exits preserve the intended reading position.
- Large EPUBs, image pages, and SD-font preparation recover more safely from limited memory and SD read errors.
- XTC/XTCH page turns no longer overlap, tables of contents show all entries, and covers retain their grayscale detail. TXT font-size controls and Home progress work reliably.
- Quick Resume, custom sleep images, transparent overlays, and X3/X4 wake refreshes avoid blank screens, grid artifacts, and lingering images.
- Long-press shortcuts no longer trigger an extra action on release; Quick Actions, Quick Lock, and Dark Mode shortcuts respond consistently.
- Touch scrolling, page gestures, font-download cancellation, and reader settings behave reliably across orientations and UI scales.
- KOReader Sync preserves orientation and settings, handles missing remote positions, and avoids repeated screen flashes during network transitions.
- Nearby sync, OPDS search, file listings, and image actions handle input and errors more reliably; Calibre Wireless shows the full IP address.
- S3 sleep, charger detection, and power-button wake behavior are more reliable. USB Drive recovers from storage failures, USB transfers avoid watchdog errors, and updates reject firmware for a different board.
- Book-specific settings stay separate from global defaults, and Recent Books and KOReader credentials survive network restarts.

## [v1.5.0] - 2026-08-08

### Added

- X4 Pro readers can lock the Home button while reading, with a Power-button shortcut to toggle it.
- End-of-book suggestions can now be opened directly by tapping their rows on touch devices.
- Quick Actions lets readers assign up to five favorite reader commands to one Power, Back, or Menu shortcut.

### Fixed

- EPUB tables now lay out a row at a time in both Incremental and Full Section indexing, keeping regular tables readable without whole-table buffering.
- Touch support for Seeed Studio Sticky
- Nearby File Transfer can send EPUB, TXT, XTC, XTCH, PNG, and BMP files directly between two CrossDink devices without a Wi-Fi network.
- Recent Books and image-file long-press actions can send files directly to a nearby CrossDink device.
- Dictionary lookup and lookup history
- EPUB books can use a dedicated SD-card dictionary font while keeping a different reader font.
- EPUB books can set a dedicated dictionary font size independently of the reader font size.
- Dictionary font and size defaults can be set globally from Settings > Reader > Font Options, with per-book choices still taking precedence.
- Reusable dictionary SD-font builder with IPA coverage and per-family ZIP packaging
- RTC-enabled devices can now choose the date format and numeric separator shown in headers from Settings > System > Device.
- The web EPUB optimizer now splits oversized chapters into memory-friendlier sections before sending them to the reader.
- Reader indexing can now use `Incremental` or `Full Section` mode globally or per book; changing modes keeps the current chapter readable and applies when the next chapter needs indexing.
- Look Up Word can now be assigned to short- and long-press Power button shortcuts.
- EPUB readers can now choose from five word-spacing levels, from normal through extra-wide.
- EPUB inline-image pages on X3 now use the grayscale-aware display base before the image grayscale overlay, reducing the moment where images appear too dark before settling.
- EPUB publisher small-caps styling now renders ASCII lowercase text as smaller capital letters without needing extra font files.
- When incremental EPUB indexing runs out of memory at the first unindexed page, the reader now silently restarts once and resumes the book with a fresh heap.

### Changed

- PSRAM-equipped readers now keep EPUB grayscale and image-cache working buffers in external memory, preserving more internal RAM for layout and reducing repeated SD reads on image pages.
- Reader font sizes now persist as actual point sizes, keeping the closest matching size when font families or installed files change.
- SD-card fonts now include the built-in reader fallback stack for common symbols, emoji, and selected CJK glyphs while retaining Noto Sans fallback coverage.
- Downloadable SD-card fonts are now rendered with the same darker anti-aliasing as the built-in reading fonts.
- Full-section EPUB indexing now prepares one-page chapters and direct jumps to a chapter's last page, while avoiding repeated checks after the next chapter is ready.
- EPUB grayscale rendering now reuses its 8 KB strip buffer across stable pages, reducing repeated heap allocation and release during long reading sessions.
- Reading progress is now saved in batches during ordinary page turns, immediately after layout changes, and when leaving a book, reducing repeated SD-card writes without carrying stale pagination into the next session.
- SD-card font discovery now waits until a custom font is selected or font settings are opened, reducing SD-card work during normal startup with built-in fonts.
- EPUB page turns using SD-card fonts now prepare the next page's glyphs while the reader is idle.
- Dictionary lookups now reuse open index files for stem matching, reducing repeated SD-card work after a miss.
- The web file manager now batches directory listings into fewer network packets, improving large-folder response time.
- Firmware releases now identify the supported device type: X3/X4 or Seeed Sticky.
- Image-heavy EPUB chapters now index by reading image headers first and extract each full image only when its page is shown.
- EPUB books with repeated byte-identical stylesheets now parse each unique stylesheet only once when building caches.
- SD-card fonts now reuse their page-sized glyph buffers, reducing heap fragmentation during long reading sessions.
- Firmware builds now prioritize usable heap over oversized system timer stacks and maximum WiFi throughput, leaving more memory for reading and network operations.
- Downloaded-font size range options now show their actual point-size ranges instead of firmware build names.
- KOReader Sync and authentication, OTA updates, and OPDS browsing now restart into a lightweight network mode that leaves reader and Home data unloaded, providing more contiguous memory for WiFi and secure connections.
- The web file manager can now delete non-empty folders recursively and, when hidden files are shown, remove hidden or system-managed SD card items after confirmation.
- SD-font, OPDS catalogs, and other unneeded settings now stay out of memory while reading unless their settings are open.
- EPUB books can now keep more saved clippings without loading every clipping's text into memory while reading.

### Removed

- The font download manager no longer offers a Download All action; fonts can still be downloaded individually or updated together.

### Fixed

- Book menu tab navigation, popup scrolling, customized Reading Stats hints, and short button presses after low-power mode now work reliably.
- Sleep screens now honor the current orientation, avoid X4 transition flashes, fall back to a valid wallpaper when needed, and handle low-memory image decoding without rebooting.
- Choosing Set Cover uses the selected image in place, and Home no longer repeatedly generates missing EPUB covers.
- Finished-book suggestions are now collected before an EPUB is moved to `/Read`.
- Manage Fonts now opens and scans large catalogs more safely on X3/X4, reports low-memory failures instead of restarting, and returns to Font Options when cancelled.
- Network screens refresh cleanly on X4; long errors wrap correctly; saved Wi-Fi networks and KOReader connections recover more reliably after restart or a missing address.
- Translated Wi-Fi and clock labels no longer truncate text or time values, and clock sync no longer risks a reboot while saving settings on memory-constrained X3/X4 devices.
- KOReader Sync no longer crashes during time setup, re-triggers while connecting, or loses precise EPUB positions; CrossPoint-only data stays on the official CrossPoint Sync server.
- Firmware updates reject images for the wrong chip family, and saved Wi-Fi settings safely handle concurrent access and corrupted values.
- EPUB opening, reflow, and background indexing now handle fragmented memory more safely, retry recoverable work, remain responsive to input and setting changes, and show useful errors instead of rebooting or silently returning Home.
- Low-memory EPUB grayscale and sleep rendering now fall back safely without leaving stale display content.
- Full-section indexing preserves more memory for large chapters and cancels speculative work on page turns, keeping the reader responsive.
- SD-card font and clipping work now release temporary data at the right time, preserving memory for reflow, dictionary use, covers, and thumbnails on X3/X4.
- EPUBs with book-specific built-in fonts no longer load an unnecessary global SD-card font, and custom fonts retain ligatures.
- EPUB styling choices apply before style caches load; CSS-heavy books use less temporary memory; and disabling Embedded Style consistently skips unused stylesheet work.
- EPUB layout now keeps CJK ruby and spaces, Russian paragraph continuations, Focus Reading, underline/strikethrough runs, and right-to-left text correct.
- EPUBs with flowing `<br>` elements, image-led or decorative chapter headings, unsupported images, and dense final pages now lay out without excess gaps, clipping, dropped images, or misleading low-memory warnings.
- EPUB footnote and cross-reference previews now show complete notes, including targets in the middle of a paragraph.
- Saved EPUB positions, clipping highlights, and selections now stay accurate after font, orientation, or indexing changes; selections also remain readable in dark mode and on memory-tight pages.
- Dictionary misses can switch dictionaries without leaving the reader, and dictionary read failures now report an error instead of a false “not found.”
- Reader popups, KOReader Wi-Fi labels, Lyra battery headers, and the sleep message now remain correctly oriented and positioned.
- Manual refreshes preserve EPUB and TXT text anti-aliasing; XTC and XTCH status bars show the configured time-left estimate.
- Watchdog panics with captured diagnostics open crash reporting, while reset-only events return normally; power-button wake timing no longer depends on SD-card startup.
- The web file manager and uploads now handle simulator/device ports and stalled connections safely; unsupported settings stay hidden, and the optimizer removes empty chapter stubs without breaking table-of-contents links.

## [v1.4.0.1] - 2026-07-28

### Added

- Updates to support Xteink device detection so the correct display panel driver is used.

## [v1.4.0] - 2026-07-10

### Added

- Dashboard UI theme for the Home screen, showing the current book cover and reading stats.
- Nearby Position Sync for sending or applying the current EPUB position between two CrossDink devices over ESP-NOW.
- Web EPUB optimizer support for CrossDink location metadata, so optimized EPUBs can keep better progress and stable page numbers.
- Reading Stats support for XTC and XTCH books, including reader menus, Home and sleep screen stats, mark finished, delete stats, and preserving stats when clearing book caches.
- Web file manager image previews, so PNG, JPEG, BMP, GIF, and WebP files can be viewed inline before downloading.

### Changed

- Large EPUBs, SD-card font-heavy books, and cover thumbnails now open, index, and generate more reliably under low-memory conditions.
- Home and sleep screens now load more cover and thumbnail data only when needed, reducing reader startup work and reusing cached cover data where possible.
- Built-in reader font choices have been reduced to Lexend Deca and Bitter, reducing firmware size while keeping fallback glyph coverage.

### Removed

- Teensy firmware builds are no longer produced for releases or release candidates.

### Fixed

- EPUB render-mode and Safe Mode toast messages now clear reliably, even when the reader is low on memory.
- EPUB Reading Stats no longer drops unsaved page-turn counts after viewing the stats screen mid-session.
- KOSync is more reliable with many SD-card fonts installed, reducing low-memory failures during secure sync requests and uploads.
- Web file manager actions now handle filenames with special characters safely and reject unsafe rename characters before saving.
- Auto Turn interval settings and related action prompts opened from long-press shortcuts now stay open after releasing the shortcut button.
- EPUB footnote previews no longer show clipped status-bar labels or misleading reader progress indicators, and clipping selection now works from footnote previews.
- Font selection no longer reopens the font preview after choosing a font.
- EPUB chapters with stale publisher style data now rebuild it instead of opening without the book's styling.
- Large SD-card font EPUBs no longer overlap characters after font or line-spacing changes, and clipping selection can fall back to a built-in UI font when needed.
- EPUB cover and thumbnail generation is more reliable with custom SD-card fonts selected and optimized books under low-memory conditions.
- Web EPUB optimizer now preserves more PNG and SVG artwork on-device, including transparent PNGs, dividers, and images in malformed or XML-declared chapters.
- Unsupported SVG images in EPUB chapters are now skipped silently instead of triggering low-memory image warnings.
- Nearby Position Sync now silently restarts back into the reader after using ESP-NOW, matching other WiFi sync flows and reducing post-sync memory fragmentation.
- EPUB page cache loading now uses fewer small heap allocations, reducing fragmentation-related reader failures.
- EPUB grayscale page turns on X3 now use the grayscale-aware display base, reducing the moment where new text appears too dark before the anti-aliased overlay finishes.
- EPUB chapters with many inline anchors, footnote links, malformed XHTML, large publisher styles, or SD-card fonts are less likely to fail or get stuck on the indexing screen.
- EPUB opening and image rendering now recover from more low-memory conditions instead of rebooting, including landscape image pages and books that need lighter render modes.
- EPUB clipping selection now follows right-to-left line order when selecting Hebrew and other RTL text.
- Lyra Carousel no longer shows a blank carousel after returning from WiFi-related File Transfer screens and moving between the menu row and book row.
- Generated SD-card font packages now include the same core glyph coverage as built-in reader fonts.
- Manage Fonts no longer crashes while loading or reloading large SD-card font lists.
- Minimal Home no longer swaps to another recent book when returning from Settings when Back button is mapped to the first button.
- Cancelling a font download now stops on the first Cancel button press instead of needing several presses.
- The `Inverted` sleep cover filter now keeps book covers unchanged on Minimal and Dashboard sleep screens while switching the background to white.
- Rare EPUB open or thumbnail crashes during ZIP decompression are fixed.

## [v1.3.4] - 2026-06-24

### Added

- File Browser now indexes large SD-card folders so directories with many books can be browsed without loading every filename into memory at once.
- EPUB text clipping with saved highlights, clipping lists, and Kindle-style `/My Clippings.txt` export.
- `Create Clipping` is now available as a reader shortcut for short/long Power, long-press Menu, and long-press Back actions.
- Per-book EPUB options for font, layout, styling, reading aids, and render modes, including `CrossDink Default`, `Balanced`, and `Light` modes for difficult books.
- Arena allocator (`lib/Memory/Arena.h`) for burst-then-discard allocation patterns - reduces heap fragmentation during EPUB parsing and page layout over long reading sessions.
- Optimized EPUBs now store location metadata at `META-INF/x-locations.json`.
- X3 SD-card writes now use the RTC for file timestamps when the clock is available.

### Changed

- The EPUB reader menu now splits the growing menu into 3 screens, labels per-book settings as `Book Options`, and avoids showing duplicate `Orientation` controls.
- The `Inverted` sleep cover filter now flips Minimal and Reading Stats sleep screens to black text on a white background.

### Fixed

- Quick Resume no longer shows a blank page after EPUB next-chapter indexing.
- Calibre Wireless transfer status no longer stacks the last received-file message on top of the upload percentage.
- X3 Tilt Direction now labels left/right choices as `Left-Right` and `Right-Left`, with existing left/right preferences migrated to keep the same physical tilt behavior.
- EPUB layout now honors publisher page-break CSS, avoids stretching justified spaces before closing punctuation, and keeps large CSS rule sets in a smaller disk-backed lookup cache.
- EPUB first-open conversion now uses more compact OPF manifest lookups and streams cover-wrapper parsing to avoid large temporary heap buffers on books with huge manifests.
- EPUB chapters that run out of memory now retry with `Balanced`, `Light`, and final `Safe Mode` rendering before showing an error, apply the same fallbacks during next-chapter pre-indexing, and let book action menus reset a book's reader settings if Safe Mode still cannot open it.
- EPUB reader font-size changes now restore the current chapter position by content instead of jumping far backward after re-indexing.
- Reading Stats now use the reader's last live book time-left estimate instead of showing a separate fallback estimate.
- Per-book reading stats now migrate compatible legacy `stats.bin` files into the `stats_v5.bin` flow instead of resetting when only the old filename exists.
- Lyra Carousel Home menu rendering now avoids extra label allocations that could crash builds under low memory.
- Lyra Carousel Home cover refresh no longer risks a reboot when memory is tight after returning to or selecting a recent book.
- EPUB image-heavy chapters no longer risk a reboot while saving their reading cache under low memory.
- TXT readers now stay open when pressing a page-turn button at the end of the file.
- Long-press reader shortcuts that open another screen no longer close or confirm it again when releasing the shortcut button.
- RoundedRaff's header battery icon and percentage now sit lower to avoid clipping at the top edge.
- Lyra Carousel now keeps the Home header current when rendering the menu or restoring cached carousel frames, preventing stale battery and clock values while navigating between books.
- Web file manager multi-delete now handles larger selections without failing after a small batch.
- Portuguese EPUBs now use Portuguese hyphenation rules instead of leaving long words unhyphenated when Hyphenation is enabled.
- Progressive JPEG EPUB covers now render more smoothly in generated cover and thumbnail BMP assets.
- EPUB section layout now flushes long text runs earlier when Focus Reading or Guide Dots are enabled, reducing low-memory failures on difficult books.
- Footnotes in EPUBs with very large shared notes sections no longer cause long stalls when opened.
- Firmware updates now follow GitHub asset redirects before streaming the install.
- Tiled grayscale rendering now serializes display transfers on the shared SPI bus to avoid display glitches during SD activity.

## [v1.3.3] - 2026-06-13

### Added

- `File Browser Display` in `Settings > System > Files & Cache` for choosing one-line or two-line file browser rows across all themes, while preserving Minimal users' existing two-line display on upgrade.
- `Hide File Extension` in `Settings > System > Files & Cache` for expanding file-browser filenames by hiding the right-side extension label.
- Device Name in Settings > System > Device for customizing the KOReader Sync and Nearby Stats Sync device label.
- Additional shortcut options and new ability to add custom shortcuts for Long-press Back Action.
- Delete Reading Stats actions in the EPUB reader and book action menus for clearing one book's stats without deleting its cache.

### Changed

- CrossDink settings now save to `/.crosspoint/crossink-settings.json`, with a one-time fallback migration from `/.crosspoint/settings.json`, so switching between firmware builds is less likely to reset preferences.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.

### Fixed

- RoundedRaff's date shown in settings now sits lower on X3 devices instead of overlapping the battery.
- Clear Bookmark List now asks for confirmation before deleting a book's bookmarks.
- Clear Reading Cache now preserves per-book reading stats while continuing to leave all-time reading stats untouched.
- Moving finished EPUBs to `/Read` now consistently preserves reading progress, per-book stats, bookmarks, and resume state.
- Book settings option lists now return to the submenu they were opened from when pressing Back.
- Lyra Carousel now refreshes its cached Home icon row after OPDS, Reading Stats, or Bookmarks icons appear or disappear.
- KOReader Sync failure screens now wrap long error messages and shut down WiFi cleanly before returning to the book.
- Sleep Screen > Cover now generates the current book cover on demand instead of falling back to the dark sleep screen when the setting is changed after opening a book.
- File Browser now previews PNG images instead of trying to open them as EPUBs, and hides common macOS and Windows metadata files.
- File Browser now refreshes immediately after falling back to the root folder from a stale saved path.
- File Browser now stops loading oversized folders before low memory can crash the device and shows a memory error instead.
- TXT reader long-press Power page turns now work when Long Power Button is set to Page Turn.
- SD-card font read failures no longer risk a reboot while cleaning up the failed file read.
- Page Overlay sleep screens no longer force EPUB chapters to re-index after waking.
- Page Overlay sleep screens now use the current screen as the overlay background outside the reader instead of trying to rebuild a stale book page.

## [v1.3.2] - 2026-06-10

### Added

- Current date in the top-right Settings header on X3 devices.
- Dark Reader Mode for EPUB and TXT reading screens, plus shortcut actions for the power button and front-button long press.
- File Browser long-press folder action for choosing a custom sleep-image folder instead of only `/.sleep` or `/sleep`.
- Expanded X3 Reading Stats, including streaks, time charts, editable dates, all-time backups, reset controls, an idle-time threshold, and the `Minimal Stats` sleep screen.
- `Reset Reading Pace` in the EPUB reader menu when Time Left is enabled, for clearing only the time-left pace estimate while keeping book reading stats.

### Changed

- Display, Reader, and Controls settings now open list menus instead of cycling through options one by one.
- The X3 clock visibility setting is now phrased as `Hide Clock`, with existing `Show Clock` preferences migrated to the matching hide behavior.
- Reading time and time-left pace tracking now ignore page intervals longer than the configured idle-time threshold.
- Web portal pages now use shared templates, stylesheet, and logo assets, reducing on-device page size and improving browser caching.
- Already-cached EPUBs now open directly to the first page without an extra book-loading popup refresh.
- Reader font-size choices now show point sizes like `10 pt` instead of names like `Tiny`.

### Fixed

- Inverted reader menus now honor orientation-aware side-button navigation.
- EPUB book time-left estimates now wait for more session pace samples and use a progress-based floor after pace data exists, reducing swings from unusually short or long pages.
- Deleting an EPUB book cache now preserves that book's reading stats and pace data.
- X3 clock settings now have clearer UTC offset editing, and `Sync Date/Time` can use saved WiFi networks automatically.
- Home, Lyra Carousel, WiFi setup, and SD-card font flows now release memory more aggressively to avoid freezes or crashes on constrained builds.
- Vietnamese settings labels no longer show replacement diamonds after generated translation offsets shifted.
- KOReader Sync now lands correctly at chapter starts and shows more specific connection guidance.
- EPUB bookmarks saved under the old unstable path hash now show up again, including for books moved to `/Read`.
- SD-card font downloads now use versioned direct S3-hosted HTTP endpoints with CRC validation, avoiding GitHub release redirects and ESP32-C3 TLS stalls when loading the font catalog.
- EPUB text blocks now keep the book's alignment style when an inline image appears before the text.

## [v1.3.1] - 2026-05-28

### Added

- EPUB reading-position improvements, including bookmark anchors, bookmark preview snippets, and optional chapter/book time-left estimates.
- Nearby Reading Stats sync with separate totals for this device and all synced CrossDink readers.
- Per-server OPDS filename settings so downloaded books can use either Author - Title or Title - Author.
- EPUB render heap diagnostics that include the largest allocatable block, not just total free heap.

### Changed

- Moved the X3 reader clock into a new top-centered status bar and moved clock settings to Settings > System > Device.
- Reworked Display, Reader, Controls, in-reader options, and larger System settings groups so related options open as submenus.
- Improved OPDS and font download responsiveness by reducing progress-update overhead and temporarily disabling WiFi power saving during transfers.
- Book selection now shows a loading popup before EPUB indexing or cache loading begins.
- Delayed the automatic finished-book prompt until the reader leaves the chapter where they reach 99%.

### Fixed

- WiFi settings screen now keeps the displayed MAC address consistent with the router-visible WiFi address.
- Reader UI issues with inverted menu button hints, Lyra Carousel popups, and Auto Page Turn interval persistence.
- Web uploads and KOReader Sync progress saves now preserve progress, stats, settings, and valid resume data for refreshed book files.
- OPDS low-memory handling now shows a specific parser-buffer memory message and releases SD-card fonts before catalog loading.
- EPUB cache, CSS, table, SD-card font, and allocation failure paths now recover, retry, or stop cleanly under low memory instead of opening unstyled pages, failing unnecessarily, or risking a reboot.
- EPUB text with invisible word-joiner characters no longer shows replacement diamonds for missing font glyphs.
- Clarified the low-memory EPUB image warning so it says some or all images may be missing.

## [v1.3.0] - 2026-05-21

### Added

- Back/Cancel support while downloading books from OPDS catalogs.
- Recent Books long-press menu in both List and Grid views with delete, cache delete, completion, and remove-from-recents actions.
- Minimal sleep screen option that shows the current book cover and reading progress on a dark background.
- More detailed WiFi connection debug logs for scans, selected networks, status changes, disconnect reasons, and timeouts.
- 9pt `Itty Bitty` reader font size, plus build flags for omitting Itty Bitty and Large reader font assets in size-constrained firmware variants.
- In-reader confirmation message when a shortcut turns tilt-to-turn on or off.

### Fixed

- WiFi and OPDS connection-flow edge cases: manual Settings connections now show the connected status before continuing, copied or corrupted saved-password files are rejected before use, OPDS retries show loading before requests, and large OPDS feeds fail safely under low memory instead of rebooting.
- Reader and Home UI polish issues, including landscape status-bar settings, missing Vietnamese labels, File Browser and Lyra Carousel icon alignment, cover thumbnail artifacts, and duplicate Home progress/stat loading.
- EPUB cache and low-memory handling now use stable cache folder keys, migrate older cache folders where possible, rebuild stale section caches, lay out very long text blocks earlier, stream table fallback content when heap is tight, and clarify the warning text.
- Sleep-entry, network, and SD-card font download reliability improvements: cached sleep-screen assets are reused, OPDS pages idle normally after load, the X3 tilt sensor sleeps outside the reader, WiFi power saving is disabled during transfers, WebDAV stack usage is lower, longer stalls are tolerated, interrupted font files are retried, and active reader fonts are freed when needed.
- Remaining reader service edge cases, including an XTC chapter selector crash on memory-constrained builds, SD-card font size selection, SD-card font-size shortcuts skipping manually installed sizes, and KOReader Sync login compatibility with self-hosted servers that return valid JSON on success.

### Changed

- Modified upstream "page-as-sleep" behavior into a new `Sleep Screen > Quick Resume` option, which also keeps `Quick Resume on Timeout` on, and renamed the timeout-only toggle.
- Improved reader and browser menu behavior by moving the Footnotes shortcut above Select Chapter, wrapping long book titles in action menus, and reducing progress-screen repaint work during OPDS and SD font downloads.

## [v1.2.11.1] - 2026-05-15

### Changed

- Removed Medium font size from `xlarge` build to get it below the size limit

### Fixed

- Lyra Carousel is now included by activating the build flag `DCROSSDINK_ENABLE_LYRA_CAROUSEL=1`

---

## [v1.2.11] - 2026-05-14

### Added

- New personal theme: "Minimal"
- Custom sleep timer picker so `Time to Sleep` can be set from 1 to 30 minutes instead of cycling fixed presets.
- In-reader Controls shortcut for customizing buttons without leaving the book.
- Bookmark cleanup shortcuts: hold Select on a bookmark to delete it, or hold Open on a book in Bookmarks to clear that book's bookmark list.
- Confirmation message after deleting a book's cache from the reader or File Browser.
- File Browser long-press action for deleting an EPUB or XTC book's cache.
- Downloaded-font size range setting so SD-card fonts can use compact, default, or large point-size sets.
- File Browser long-press action for marking EPUB books as finished or unfinished.

### Changed

- Hardened deep sleep entry by shutting WiFi down before waiting for the power button to be released.
- Raised the web file-transfer filename limit from 100 to 150 bytes so longer uploaded filenames are preserved.
- Made the in-reader Reader Options menu include the same Reader settings and actions as Settings > Reader.
- Split SD-card font descriptions and supported languages into separate lines in the font download screen.

### Fixed

- Inline EPUB images no longer disappear in landscape when their bottom edge slightly overlaps the screen margin.
- Reduced unnecessary low-memory image suppression for JPEG-heavy EPUB chapters and added CSS heap diagnostics during chapter rebuilds.
- Allowed wider inline JPEG images in EPUBs to render when they still fit the total pixel and heap safety limits.
- SD-card font picker no longer reopens immediately after selecting a font from Settings > Reader > Font Family.
- In-reader font-size changes now work for SD-card fonts.
- In-reader SD-card font changes now rebuild the current EPUB page layout consistently.

## [v1.2.10] - 2026-05-11

### Added

- `Recent Books View` setting so the dedicated Recent Books screen can switch between the classic list and a 3x3 cover grid.
- More flexible reader controls, including orientation-aware front/side button settings, nav-only or all-button front inversion, tilt page turn shortcuts, and side-button long-press rotation actions.
- Per-session auto page turn interval picker with values from 5 to 120 seconds.
- File Browser Home/Back long-press action for toggling hidden files and folders.
- EPUB rendering and diagnostics improvements, including visible `<hr>` separators and heap logs around section rebuilds, image extraction, page serialization, and sleep-cache rebuilds.
- Reader font coverage for block redactions, black-square ornaments, Greek category letters, and turned-comma punctuation (PR #104).
- Simulator tools for testing sleep/wake behavior and smoke-testing common screens and EPUB reader menus.

### Changed

- Reduced Controls settings section spacing so the grouped controls fit better on X3 screens.
- Made front reader long-press actions trigger when the hold delay is reached while normal page turns still trigger on release.
- Used the fast EPUB spine/TOC indexing path for books with 300+ spine entries so heavily split books build `book.bin` faster on first open.
- Allowed the web file manager and WebDAV to browse dot-prefixed hidden files when hidden files are enabled, matching the device file browser.

### Fixed

- Reader button and shortcut behavior, including X3 power-button wake filtering, folder delete long-press timing, and WiFi scan/connect screens that could not be exited while work was in progress.
- RoundedRaff home-menu, keyboard, and button-hint rendering issues so Settings remains reachable and compact labels no longer overlap or disappear.
- Font and glyph handling now reduces persistent SD-card font advance-cache memory, releases optional font caches before image extraction only when heap is tight, and shows a visible replacement symbol when compact UI fonts lack `U+FFFD`.
- KOReader Sync authentication diagnostics and an in-reader sync crash, including clearer handling when a server or proxy returns non-JSON content.
- EPUB text rendering for redactions, whitespace-only XHTML text nodes, simple black CSS span backgrounds, list bullets in `<li><p>...</p></li>` items, and very long base64-like text runs.
- EPUB image, thumbnail, and section-rebuild stability so image-heavy chapters use less temporary memory, scale images more reliably, avoid stale dimensions, and suppress optional image work earlier under heap pressure.
- EPUB low-memory and cache safety now skips optional next-chapter indexing and sleep-page cache rebuilds when heap is tight, fails safely with a malformed-book warning and Home exit path, rebuilds incompatible fork-written caches, and handles low-memory CSS parsing, truncated SD writes, invalid serialized strings, and failed temp-cache promotion.
- Home no longer crashes after clearing reading cache when the source EPUB cache is missing.
- Reader prewarm behavior now skips image decoding, keeps mixed-style font glyphs cached together, and avoids section rebuilds for render-quality-only option changes.
- Concurrent render/storage crashes are avoided by serializing `GfxRenderer` scratch-buffer access, shared SPI bus access, and failed SPI lock cleanup.
- Recent Books, EPUB/XTC thumbnail caches, deleted-folder metadata, and XTC cover scaling now keep cached book data in sync and grid covers fill their slots correctly.
- Simulator build configuration now lets SDL2 and simulator-provided network/OTA shims compile cleanly.

---

## [v1.2.9.1] - 2026-05-03

### Changed

- Cleaned up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed

- Power-button shortcut conflicts outside the reader so reader-only actions fall back to `Confirm` while Sleep, Refresh, Screenshot, Sync Progress, and File Transfer remain real power actions.
- Potential crash when using `Go to %` in EPUBs.
- Potential crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid.
