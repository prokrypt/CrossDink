---
title: Data Cache
nav_order: 16
---

# Data Cache

CrossDink caches data aggressively on the SD card to minimize RAM use. Its ESP32-S3 readers add PSRAM, but their fast internal RAM is still limited, so rebuilding every book structure in memory on every open would be too expensive.

The main data directory is `.crossdink` on the SD card. It stores render caches and persistent user/device data.

CrossInk's `/.crosspoint` is read in place: a `/.crossdink` file or folder that does not exist yet is read from the same path under `/.crosspoint`, and folder listings show the entries of both. Every write goes to `/.crossdink`, so each file moves over the first time it is saved (an in-place update of a file only `/.crosspoint` has copies that one file first). Removing a path that `/.crosspoint` also has adds it to `/.crossdink/.deleted`, so the old copy stops showing. Atomic saves (the live file parked as `<file>.bak` while the new one is written) do not add to it: the `.bak` hides the old copy meanwhile. At the first lookup after boot, file entries whose `/.crossdink` file exists again are dropped from the list. Temp files, EPUB cache folders and caches (book.bin, sections, covers, thumbnails, image caches, the Home carousel, font catalog, sleep frame and file index) are not read through; they rebuild. Of an XTC/TXT cache folder only its user data shows through: progress, reader settings and stats. EPUB caches are named by content (`epub_<FNV-1a 64 of the file size and its last 16 KB>`), so the same user data is copied from the book's path-named `/.crosspoint` folder the first time it is opened. `/.crosspoint` is left as it was for CrossInk and older builds. Each EPUB cache folder has a `path.txt` naming its book.

## Directory Layout

```text
.crossdink/
├── global_stats.bin        # All-time reading stats, including total books read
├── global_stats.bin.bak    # Backup used if the main global stats file is corrupt
├── synced_stats/           # Stats snapshots received from other readers
├── settings.json           # CrossDink device settings
├── settings.bin.bak        # Legacy binary settings file after migration, if present
├── state.json              # Last-opened book and sleep/session state
├── state.bin.bak           # Legacy binary state file after migration, if present
├── recent.json             # Reading history for Home and Library
├── library.idx             # Library titles, authors, paths and sort indexes
├── recent.bin.bak          # Legacy binary recent-books file after migration, if present
├── wifi.json               # Saved Wi-Fi networks
├── opds.json               # Saved OPDS servers
├── koreader.json           # KOReader sync credentials
├── bookmarks/              # Bookmark files, one per book
├── clippings/              # EPUB clipping/highlight files, one per book
├── home_carousel_cache.bin # Lyra Carousel home-screen snapshot cache
├── sleep_frame.bin         # Temporary sleep overlay framebuffer, when used
├── epub_12471232/          # Each EPUB is cached to epub_<hash>
│   ├── progress.bin        # Reading position slot A (chapter, page, etc.)
│   ├── progress.bin.bak    # Reading position slot B; the newer valid slot wins
│   ├── stats.bin           # Legacy per-book reading stats
│   ├── stats_v5.bin        # Version 5 per-book reading stats
│   ├── reading_stats_off   # Present when stats tracking is disabled for this book
│   ├── reader_settings.bin # Per-book reader settings, render mode, and auto-page-turn interval
│   ├── cover.bmp           # Book cover image, once generated
│   ├── cover_absolute.bmp  # Four-tone image-mode cover (generated separately)
│   ├── cover_crop_absolute.bmp # Cropped four-tone image-mode cover
│   ├── thumb_*.bmp         # Home/recent-books thumbnail images
│   ├── book.bin            # Book metadata, spine, table of contents, etc.
│   ├── css_rules.cache     # Parsed CSS rules
│   └── sections/           # Pre-rendered chapter/page layout data
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── xtc_12471232/           # XTC progress and generated cover/thumb images
└── txt_12471232/           # TXT progress, page index, and generated cover image
```

Four-tone sleep covers use separate `_absolute.bmp` files so older cover shading is not reused. These are generated on demand for supported displays; TXT/Markdown JPG covers also use a separate `cover_absolute.bmp`. Existing cover and thumbnail caches remain available, and no manual cache reset is needed.

## Clearing Cache Data

Deleting the entire `.crossdink` directory resets caches, settings, saved network/server data, bookmarks, recent books, reading progress, and reading stats.

To clear EPUB/XTC render caches from the device UI without deleting settings or global stats, use:

**Settings > System > Files & Cache > Clear Reading Cache**

## Book Moves And Cache Identity

EPUB cache folders are named by the book's content, not its path, so moving or renaming an EPUB, even on a computer, keeps its cache folder, including reading progress, per-book reader settings, and stats. Replacing an EPUB with different content starts a new cache folder. XTC, TXT, and Markdown cache folders are still named by the file path: moving one of those books outside CrossDink can create a new cache directory, so the moved copy may start with fresh reading progress. Bookmarks and clippings are also stored by book path. CrossDink migrates them when the built-in move-to-Read flow moves an EPUB, and migrates them along with XTC/TXT caches when you rename a book in File Browser.

EPUB reader font, page layout, styling, and reading-aid settings normally come from the global Reader settings. Changes made inside an EPUB override only the fields whose values differ from the global defaults; the other fields continue to inherit later global changes. EPUB render mode is stored separately per book so a problematic title can be switched to Balanced or Light rendering from the File Browser or Recent Books long-press menus before opening it. Older full-snapshot book overrides retain their original behavior until reset or edited again.

EPUB clippings and highlights live outside the EPUB render-cache folder in
`/.crossdink/clippings/`. Each book gets a binary clipping file named from the
book type and the CRC32 of the book path. The same clipping record powers the
in-reader highlight, the clipping list, and jump-back behavior. CrossDink also
appends a Kindle-style text export to `/My Clippings.txt` on the SD-card root;
that export is human-readable and append-only, so deleting a clipping in the UI
removes the in-app saved clipping but does not rewrite old text already exported
to `/My Clippings.txt`.

Cache data is cleared by supported CrossDink delete/move flows. If you remove or rename books outside CrossDink by editing the SD card directly, old cache folders may remain until you clear reading cache.

All-time reading stats can also be backed up outside `.crossdink` in:

```text
/.crossink-stats-backup/
```

For binary file layout details, see [File Formats](./file-formats.md).

## Library

Library replaces the Recent Books screen.

The Library reconciles its index with the SD card on the first visit after boot,
after file changes in File Browser, and after leaving a download or file-transfer
screen. Ordinary return visits reuse the index. Failed or memory-limited scans are
retried on the next visit. The Library's refresh action always scans again; use it
if files were changed externally while the firmware stayed running.
Unchanged books reuse their cached metadata.
**Settings > Display > Use Book Metadata** selects embedded EPUB titles and
authors; disabling it uses filenames. TXT, Markdown and XTC files use filename
fallbacks. CLX1 version 2 adds a first-name author permutation; older Library
indexes rebuild automatically when Library opens.

**Date Added** uses the file creation time captured by the index. Its
first-seen sequence breaks ties and orders books without a creation time among
themselves; those books appear before dated books in ascending order.
The creation time is the filesystem's best available estimate of when a file
arrived on the card; some copy tools may preserve the original timestamp.
**Recently Opened** shows only books in the
saved reading history (up to 18 books), newest first. Reversing that sort shows
the same books oldest first. Marking a book unfinished does not add it to
reading history; opening it does.
Search matches words in the title and author and retains the selected sort.
The sort method and direction are saved when changed. The Library Settings icon
opens a compact/expanded list toggle and independent visibility switches for
EPUB, XTC/XTCH, TXT, and Markdown files. Expanded Title and Author sorts show
alphabetic headings; hidden file types remain indexed and can be shown again
without a rescan. Markdown books open as plain text in the TXT reader.
Expanded is the default for new Library preferences; a saved Compact choice remains respected.
Author (Last Name) sorts by the final word of the displayed author, while
Author (First Name) sorts by the displayed name from its beginning.
Series metadata and series sorting are not part of this UI change.

On button devices, Up from the first book reaches sort direction, sort method,
settings, search, and refresh. Confirm activates the selected control; holding Confirm on
a book opens its actions. On touch devices, tap the header icons or sort controls
directly and hold a book row for its actions. The sort method opens a modal list.

The Lyra Carousel snapshot cache advances to version 7 so cached home menus
regenerate with the Library label and landmark icon. No manual EPUB cache reset is required.
