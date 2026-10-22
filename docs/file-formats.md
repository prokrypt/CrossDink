# File Formats

These formats describe the SD-card cache files under `/.crossdink/epub_<hash>/`.
All POD fields are written in the ESP32 little-endian representation used by
`Serialization.h`; strings are length-prefixed UTF-8 unless a format notes a
fixed-size char buffer.

## Settings: word and character spacing

`wordSpacing` keeps saved values `0..4`. Values `5..8` encode levels `-1..-4`;
sliders show levels `-4..4`. Negative levels remove 20% of the natural word gap
per step, keeping at least one pixel. Positive levels keep their 10-pixel steps.

`characterSpacing` is `0..10`, encoding `-5..+5` (default `5`, no change). Each
step adds or removes half a pixel between letters; spaces keep their word
spacing, and ligatures are skipped while it is not `0`. It is part of the EPUB
section layout identity and the TXT index header, so a changed value rebuilds
those caches around the saved text position.

## TXT `index.bin`

Version 5 adds signed `characterSpacing` (-5..5) after the source file size in
the index header. A changed spacing value or older version rebuilds pagination.
## `/.crossdink/home_carousel_cache_<index>.bin`

### Version 9

Each Lyra Carousel position has a disposable snapshot containing only cover
artwork, titles and position dots. Progress, reading time, the KOReader Sync
badge, header, menu icons and button hints are drawn live after restoration.
Each file contains a `CarouselCacheHeader` followed by one full framebuffer. The
header's `frameCount` records the number of recent books used to compose the
artwork, rather than the number of stored frames. The key tracks Dark Mode and
the ordered book paths, titles, cover paths and thumbnail availability, rather
than reading progress or statistics.

Frames are rendered and saved only when viewed; returning Home does not prepare
other positions in advance. Version 8's combined `home_carousel_cache.bin` is
removed after the first successful write. Cache regeneration is automatic; EPUB
layout caches and reading history are unaffected.

## `/.crossdink/ttf-rendering.json`

This user-owned JSON file stores only custom TTF families whose raster settings
differ from CrossDink's defaults. Each entry is keyed by the installed family name
and contains numeric hinting, raster, interpreter, weight, and slant choices plus
the stem-darkening toggle. Missing families use automatic hinting, grayscale
output, the default interpreter, and no outline adjustments. The file currently
keeps at most 24 modified family profiles to bound RAM use while settings are open.

## `/.crossdink/sleep-image-index/<directory-hash>-{bmp,all}.idx`

### Version 1

`ImageFolderIndex` (`src/activities/boot_sleep/ImageFolderIndex.{h,cpp}`) keeps
a compact, rebuildable index per indexed folder. It backs both the sleep-image
folder and the boot-screen folder (`/bootscreen` or `/.bootscreen`); the
directory name predates the boot-screen use and was kept as-is so existing
sleep caches aren't orphaned by an unrelated rename. The index avoids walking
a folder on every selection while using only one fixed-size record at a time
in RAM. `bmp` contains BMP files and `all` contains BMP and PNG files (sleep's
Page Overlay mode only; the boot screen is BMP-only and always uses a `bmp`
index). The `validated` header flag means BMP headers were checked while
rebuilding after a failed render.

The index is disposable: a missing, malformed, or stale selected entry causes
one rebuild and then the caller falls back to its directory scan. File
transfer, file-browser, and preferred-folder changes invalidate affected
indexes via `ImageFolderIndex::invalidateForPath()`. Files added or changed
directly on the SD card have no notification path; they are picked up when a
cached entry is found missing or when an index is otherwise rebuilt.

```c++
struct ImageFolderIndexHeader {
    char magic[4];       // "CSIX"
    u8 version;          // 1
    u8 flags;            // bit 0: BMP+PNG, bit 1: BMP headers validated
    u16 pathLength;
    u16 recordCount;
    u16 recordSize;      // sizeof(ImageFolderIndexRecord)
    u32 recordsOffset;   // sizeof(header) + pathLength
    char directory[pathLength];
};

struct ImageFolderIndexRecord {
    u16 nameLength;
    u8 flags;             // bit 0: PNG (otherwise BMP)
    u8 reserved;
    char name[256];      // zero-padded UTF-8 filename, max 255 bytes
};
```

## `/.crossdink/library.idx`

### Version 8

Version 8 keeps the version 7 layout but invalidates extracted metadata so EPUB3
series refinements resolve by collection ID. The next Library scan reparses EPUB
metadata when enabled; first-seen history and each book's EPUB content key (and
so its reading progress) carry over unchanged.

### Version 7

Each book's name blob now ends with a `uint64_t` EPUB content key after the
series position: the name of its `epub_<key>` cache folder, 0 for other formats
or an unreadable file. Unchanged books keep their key across rebuilds; a book
whose content changed carries its progress and stats from the old key's cache.
Version 6 indexes rebuild on the next Library scan, reusing metadata.

### Version 6

Each book's name blob now ends with a `uint32_t` series position after the
length-prefixed genre. The value is a sortable encoding of a finite signed
IEEE-754 single-precision number; `0xFFFFFFFF` means missing or invalid. Series
sorting compares folded series name, then this position, then title order. Missing
positions follow numbered books within a series. Calibre `series_index` and
EPUB 3 `group-position` supply the value. Version 5 and earlier indexes rebuild
on the next Library scan; EPUB metadata is reread when enabled to obtain the new
field, while `firstSeen` history survives reconciliation.

### Version 5

Date Added now uses the filesystem creation timestamp. A title-ordered array of
`uint32_t` packed FAT creation date/times follows the five `uint16_t` sort
permutations and precedes the aligned name section. A zero value means the
creation timestamp is unavailable; `firstSeen` orders books with equal or
missing timestamps, and zero sorts before dated values in ascending order. The
128-byte book record still stores modification time
separately for EPUB metadata freshness. Version 4 indexes rebuild on the next
Library scan, retaining metadata and `firstSeen` values from the old index.
Header flag bit 2 marks an arrival order that fell back to `firstSeen` because
sorting by creation time ran out of memory; the next scan retries it. Version 4
uses only bits 0 and 1, so its flags remain valid during reconciliation.

### Version 4

Two more `uint16_t` permutations follow arrival order: series order and genre
order. Both use folded EPUB metadata, place missing values last in ascending
order, and use title order to break ties. The header, records, and name blobs
remain compatible with version 3. During the one-time rebuild, version 3
metadata and `firstSeen` arrival history are reused for unchanged books.

### Version 3

The Library index adds two length-prefixed fields after source author in each
book's name blob: series and genre. The fixed-size header and record layout stay
the same. A version 2 index is read once during rebuilding so existing
`firstSeen` values survive; the new index is then written as version 3.
Series and genre are read only from EPUB metadata. The library treats the first
`dc:subject` value as genre, and reads Calibre or EPUB 3 series metadata.

### Version 2

Version 2 added EPUB title and author metadata, the file modification time,
and source-author spelling to the original Library index. Its header and
128-byte records are compatible with version 3 reconciliation; its name blobs
do not contain series or genre.

### Version 1

`LibraryIndexFile` (`lib/LibraryIndex/LibraryIndexFile.{h,cpp}`) reads the
`CLX1` on-disk index for the Library screen: one sorted, searchable
snapshot of up to 32,767 books on the card, built by `LibraryBuilder` so paging,
sorting, and searching the shelf cost a handful of seeks instead of a
directory walk per screen. The format itself (`lib/LibraryIndex/LibraryFormat.h`)
is free of `HalStorage` and Arduino so its layout and validation rules are
host-testable (`test/library_format`, `test/library_index_file`).
The builder enforces a device limit below that ceiling: 32,767 books with
PSRAM, where its per-book sort arrays live, and 4,096 books without it (the
simulator). Readers accept any valid count. If the scan finds another book
beyond the limit, the rebuild fails and keeps the previous index instead of
publishing a partial shelf.

Every section starts on a 512-byte boundary. Records are a fixed 128 bytes
each, so record `k` always lives at `recordStart + 128*k` with no offset table
to load first, and 32 records exactly fill a 4096-byte scan buffer. Five
`uint16_t` permutation arrays (surname, first name, arrival, series, then genre
order) and one `uint32_t` creation-time array let those sorts page without
re-sorting on every open;
Title order needs no permutation because the record section is already
title-sorted.

The index is disposable: a bad magic, an unknown format/fold version, a size
mismatch (a build interrupted by power loss cannot pass, since `selfSize` is
checked against the real file size), or an inconsistent section layout all
cause a full rebuild rather than a crash or silently wrong output. A fold
version bump alone (the text-normalisation rules changed) can be handled by
reconciliation instead: `openForReconciliation()` accepts stale sort/search
keys so each book's `firstSeen` arrival order survives across the rebuild
even though its fold and permutations are regenerated.

CrossDink's format version is `6`; older indexes rebuild automatically. Versions
2 through 5 can be read for reconciliation so arrival history survives. The fold
version is `3`.

```c++
struct ClixHeader {            // 64 bytes, padded to the first 512-byte sector
    char magic[4];              // "CLX1"
    u8 formatVersion;           // 6
    u8 foldVersion;             // 3
    u8 flags;                   // bit0: ranks degraded, bit1: dedup degraded, bit2: arrival degraded
    u8 metadataEnabled;         // 0 or 1
    u16 bookCount;
    u16 folderCount;
    u16 nextFirstSeen;
    u16 padding1;
    u32 folderStart;
    u32 folderLen;
    u32 recordStart;
    u32 permStart;
    u32 nameStart;
    u32 nameLen;
    u32 selfSize;                // expected total file size; truncation guard
    u8 reserved[20];
};

struct ClixRecord {             // exactly 128 bytes; record k @ recordStart + 128*k
    u32 nameOff;                 // offset into the name blob, from nameStart
    u32 fileSize;                // captured while the dirent was open
    u16 firstSeen;
    u16 folderId;                // index into the folder table
    u8 nameLen;
    u8 foldLen;
    u8 authorKeyLen;
    u8 metadataStatus;           // 0 not attempted, 1 extracted, 2 failed
    char fold[96];               // folded sort/search key
    char authorKey[12];          // order-insensitive author identity
    u32 modificationTime;        // packed FAT date/time, or 0 if unavailable
};

struct ClixFolderHeader {        // one per indexed folder, back to back
    u8 pathLen;                  // 1..255; path bytes follow, no trailing '/'
};
```

The name blob for each record (found via `nameOff` into the `names` section)
holds, back to back: an 8-byte FNV-1a path hash of the book's complete path
(the identity used by rebuild reconciliation and by "is this book already in
the index" lookups), the filename, then five length-prefixed fields —
display author, title, the pre-spelling-harmonisation source author, series,
and genre. Version 6 appends the four-byte series position; version 7 then the
eight-byte EPUB content key.

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 10 rebuilds metadata with namespace-aware OPF parsing so optimizer-generated
XML prefixes do not leave an empty chapter list. The binary layout is unchanged.
Version 9 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts. It also rebuilds metadata after
the EPUB guide start-reference handling changed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `reader_settings.bin`

### Version 13

Each EPUB cache directory may contain `reader_settings.bin`. Missing files mean
the book uses global Reader settings and the default auto-page-turn interval.

Version 1 stored only:

- `u8 version`
- `u16 autoPageTurnSeconds`

Version 2 stores flags before the full reader-settings snapshot. Version 3 adds
the EPUB word-spacing level to that snapshot. Version 4 adds the EPUB indexing
method (`0` = incremental, `1` = full section). Version 5 appends a per-book
dictionary SD-font family name. Version 6 stores reader font sizes as physical
point sizes, version 7 appends the dictionary font's selected point size, and
version 8 splits the screen margin into vertical and horizontal values. Version
9 removes the obsolete per-book Dark Mode byte: Dark Mode is now a global
display setting. Version 10 appends a field mask so a book overrides only the
reader settings that differ from its current global defaults. Version 2-9
records with the custom-settings flag keep their full snapshot as an override
when migrated; they cannot distinguish past manual edits from automatic ones.
Version 12 follows CrossInk's version 12 byte layout so records carried over
from a CrossInk card load: the SD-font mask bit moves from 18 to 19, a `u8`
image-grayscale byte follows the mask (CrossDink writes `1` and ignores it,
along with mask bit 18), then `u8 characterSpacing` (0-10 encodes -5..+5,
default 5) with override bit 20. Version 10 records keep their overrides and
inherit global character spacing. CrossInk's version 11 is not read.
Version 13 follows CrossInk's version 13: it appends `u8 publisherFonts` (0 Off,
1 Ignore Body Font, 2 All Text) with override bit 21. Older records inherit the
global publisher-font mode.
The file can preserve an auto-page-turn interval without forcing custom
font/layout settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Focus Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 12
    u8 flags;   // bit 0 = at least one custom reader field, bit 1 = custom auto-page-turn interval, bit 2 = render mode override, bit 3 = dictionary font override, bit 4 = Safe Mode override
    u16 autoPageTurnSeconds;
    u8 renderMode; // 0 = CrossDink Default, 1 = Balanced, 2 = Light

    u8 fontFamily;
    u8 readerFontPointSize; // physical point size; versions 2-5 stored a size slot
    u8 lineHeightPercent;
    u8 wordSpacing; // 0 = natural; 1-4 add 10px per level; 5-8 encode -1..-4 (20% tighter per level)
    u8 orientation;
    u8 screenMarginVertical;
    u8 screenMarginHorizontal;
    u8 publisherPageNumbers;
    u8 paragraphAlignment;
    u8 embeddedStyle;
    u8 hyphenationEnabled;
    u8 textAntiAliasing;
    u8 imageRendering;
    u8 extraParagraphSpacing;
    u8 forceParagraphIndents;
    u8 focusReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    u8 indexingMethod; // 0 = incremental, 1 = full section, 2 = incremental (mental)
    char sdFontFamilyName[64];
    char dictionarySdFontFamilyName[64]; // meaningful only when flag bit 3 is set
    u8 dictionaryFontPointSize; // 0 = follow reader size
    u32 readerSettingsOverrideMask; // bits 0-17 = snapshot fields excluding snapshotRenderMode; bit 18 = unused (CrossInk image grayscale); bit 19 = sdFontFamilyName; bit 20 = characterSpacing
    u8 imageGrayscale; // CrossInk only; CrossDink writes 1 and ignores it
    u8 characterSpacing; // 0..10 encodes -5..+5
};
```

## Sync files

`sync_pending.bin`, `sync_state.bin` and each book's `sync.bin` share one
crash-safe pattern (`src/sync/SyncFiles.cpp`). A change writes `<name>.tmp`,
closes it, renames the current file to `<name>.bak`, then renames the temp file
into place. When there is no current file (first write, or recovering from the
backup) the temp file is renamed straight into place and `.bak` is kept. Every
file ends with a little-endian `uint32` CRC32 (`uzlib_crc32`) of all bytes
before it. A reader checks the CRC first. If the main file is missing but a
CRC-valid `.tmp` exists (power lost between the two renames), that newest copy
is renamed into place and used; otherwise it falls back to `.bak`. If nothing
valid remains the file starts fresh (see below). Integers are little-endian.

### `/.crossdink/sync_pending.bin` (version 1)

Books whose local state changed since their last successful upload.

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| entries | repeated | until the CRC |
| entry.mask | `uint8` | `1` progress, `2` reading stats, `4` clippings |
| entry.path | `uint16` length + bytes | absolute book path, at most 1024 bytes |
| crc | `uint32` | |

Entries are only added while a sync account exists, and stats/clippings bits
only while their What to Sync toggle is on. An entry is removed when its mask is
empty. The file is never deleted; an empty list is just the version byte. If it
and its backup are both unreadable after a catch-up has completed, the catch-ups
in `sync_state.bin` are reset so they run again.

### `/.crossdink/sync_state.bin` (version 2)

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| baseUrl | `uint16` length + bytes | account this state belongs to |
| username | `uint16` length + bytes | |
| flags | `uint8` | bit 0 stats caught up, bit 1 clippings caught up |
| statsCatchupId | `uint32` | non-zero while a stats catch-up is unfinished |
| clippingsCatchupId | `uint32` | same for clippings |
| nextStampId | `uint32` | next catch-up/walk id; random start, never reused |
| scopes | repeated | version 2: prepared sync scopes, until the CRC |
| scope.path | `uint16` length + bytes | folder, or `/` for the whole card; at most 255 bytes |
| scope.flags | `uint8` | bit 0 prepared, bit 1 stats caught up, bit 2 clippings caught up |
| scope.cursor | `uint32` | change feed cursor for this scope |
| scope.lastSynced | `uint32` | stamp id of the last sync; the oldest is dropped beyond 32 scopes |
| scope.walkId | `uint32` | non-zero while the scope's first walk is unfinished |
| scope.statsCatchupId | `uint32` | unfinished stats catch-up for this scope |
| scope.clippingsCatchupId | `uint32` | unfinished clippings catch-up for this scope |
| crc | `uint32` | |

Version 1 files (no scopes) are read and rewritten as version 2. A different
base URL or username reads as a fresh state (catch-ups and first walks run
again). Scopes are streamed one at a time, so RAM use does not grow with the
list.

### `/.crossdink/sync_ids.bin` (version 1)

Document id -> book path for books with saved progress, stats or clippings on
this reader, read by the progress pull. XTC books and finished books are never
recorded. Unlike the other sync files it is appended to, so each record carries
its own CRC and there is no trailing file CRC; rewrites (rename, removal,
compaction) still use the `.tmp` swap. A confirmed Document Matching change
deletes it, together with `sync_parked.bin` and every scope's cursor, first walk
and catch-ups in `sync_state.bin`.

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| method | `uint8` | `DocumentMatchMethod`; another method reads as an empty map |
| generation | `uint32` | changes whenever the file is created anew, compacted, or rebuilt after a corrupt entry |
| headerCrc | `uint32` | CRC32 of the 6 bytes above |
| entry.id | 16 bytes | raw KOReader document MD5 |
| entry.path | `uint8` length + bytes | absolute book path, at most 255 bytes |
| entry.crc | `uint32` | CRC32 of the entry's id, length and path |

A torn append only loses that entry: the next record finds the partial tail and
rewrites the file before appending. If the main file is missing after a crash
between the two renames, a `.tmp` with a valid header is promoted, else the
`.bak`. Copies of a file in binary mode share an id, so an id can map to several
paths.

### `/.crossdink/sync_parked.bin` (slot version 1)

Remote positions from the change feed that the pull could not apply: books not
on this reader (or not in `sync_ids.bin`), books never opened here, failed
applies, and Ask on conflicts' Skip book. At most 32 fixed 384-byte slots; a
change sharing a hash with a parked one replaces it in place, else the first
empty slot is used, else the oldest (by remote timestamp) is overwritten. A slot
whose CRC does not match (torn write, or zeroed by a removal) is empty. There is
no file header or file CRC. Removing the last valid entry deletes the file, so
opening a book with nothing kept stops at the missing file.

| Offset | Field | Type | Notes |
| --- | --- | --- | --- |
| 0 | version | `uint8` | `1`; `0` = empty slot |
| 1 | hashCount | `uint8` | 1-4 |
| 2 | flags | `uint8` | bit 0: rich position present |
| 3 | reserved | `uint8` | |
| 4 | hashes | 4 x 16 bytes | raw document MD5s: canonical first, then aliases |
| 68 | percentage | `float` | |
| 72 | timestamp | `int64` | remote write time |
| 80 | pctQ | `uint32` | rich position |
| 84 | spine, page, pages, paragraph | 4 x `uint16` | rich position; paragraph 0 = none |
| 92 | device | 32 bytes | remote device name, NUL-padded |
| 124 | xpath length | `uint16` | at most 252; longer positions are not parked |
| 126 | xpath | 252 bytes | |
| 380 | crc | `uint32` | CRC32 of bytes 0-379 |

### `<book cache>/sync.bin` (version 2)

In the book's `epub_<hash>/` or `xtc_<hash>/` folder; only written when that
folder already exists.

| Field | Type | Notes |
| --- | --- | --- |
| version | `uint8` | `1` |
| hasId | `uint8` | `1` when `id` is valid |
| method | `uint8` | `DocumentMatchMethod` the id was computed with |
| id | 16 bytes | raw KOReader document MD5 |
| fileSize | `uint32` | book size when `id` was computed; a different size recomputes it |
| statsStamp | `uint32` | stats catch-up id that last sent this book |
| clippingsStamp | `uint32` | clippings catch-up id that last sent this book |
| walkStamp | `uint32` | version 2: first-walk id that finished this book |
| idsGeneration | `uint32` | version 2: `sync_ids.bin` generation the book was recorded in; 0 = not recorded |
| crc | `uint32` | |

Version 1 records are read with both new fields 0.

Nothing writes these files yet: the bulk sync screens that use them come in a later change.

## Daily reading counters (v2)

Device-local counters live in `/.crossdink/daily_reading/YYYY-MM.a.bin` and
`YYYY-MM.b.bin`. Each month uses at most two 264-byte snapshots, regardless of
how many days were read (dates support 2000–2099). All integers are little-endian:

| Offset | Size | Meaning |
| --- | --- | --- |
| 0 | 4 | Version 2, ASCII `D`, ASCII `R`, reserved zero |
| 4 | 4 | Month's first day index since 2000-01-01 |
| 8 | 4 | Generation, incremented for each snapshot (wrap-safe comparison) |
| 12 | 248 | 31 pairs of uint32 cumulative seconds and acknowledged seconds |
| 260 | 4 | FNV-1a checksum of bytes 0–259, initial hash 2166136261 |

Records after the month's last day must be zero. Acknowledged seconds must not
exceed cumulative seconds. Saves overwrite the older/invalid slot, sync and close
it, then reopen and validate it. Reads choose the newest valid generation. A torn
write falls back to the other slot. New months first establish an empty generation-0
`b` snapshot before creating `a`; a lone torn `b` is an unfinished initialization
and can be retried. Unknown newer formats and two corrupt copies are preserved and
rejected. Unchanged counters do not cause writes. CrossDink never wrote upstream's
per-day tester files (`NNNNN.bin`), so they are not read or converted.

Only accepted page intervals in the existing EPUB/XTC stats paths contribute, with
the existing idle threshold and ten-second session minimum. Each accepted interval
captures the wall clock at its start and end; midnight splits actual intervals.
An unset RTC, a changed UTC offset setting, or wall time changes over two seconds
leave that interval undated. Local dates follow `LocalClock` (daylight saving
included).

Accepted seconds stay in RAM (two day counters, plus at most nine seconds of
pending short-session intervals per reader) and are written only at the held-write
flush points (`ReaderExitSave::flush()`: sleep, restart, any screen but Home, Library and the readers,
book actions, low battery, the remote FLUSH), never on a page turn. Reading on a
third day before any flush writes the oldest day out first. A sudden power loss
loses the seconds since the last flush, as it does the other held reader writes.

Stats sync extends `PUT /api/v1/stats/global` with an optional `daily` array:
`[{"date":"2026-10-01","seconds":61}]`. Dates are local calendar dates. One
changed day goes per request in the 1536-byte payload buffer, marked uploaded
only after `accepted_daily: 1`. After a complete daily upload, fully acknowledged
months older than the previous calendar month are deleted; months with unuploaded
seconds, or any corrupt or unreadable snapshot, stay.

## `/.crossdink/clippings/<bookType>_<crc32(path)>.bin`

### Versions 1-4

Clipping files store the per-book EPUB clipping list used by the reader. A
saved clipping is also what CrossDink renders as an in-reader highlight; there is
no separate highlight file. The file lives in `/.crossdink/clippings/` instead
of the EPUB render-cache directory so clearing/rebuilding layout cache does not
delete user clippings.

The current implementation only writes EPUB clipping files, so `bookType` is
`epub`. The numeric suffix is `uzlib_crc32()` of the book's SD-card path, for
example:

```text
/.crossdink/clippings/epub_1234567890.bin
```

Binary layout:

- `[0]` version (`1`, `2`, `3`, or current version `4`)
- `[1-2]` clipping count (`uint16_t` LE, maximum `256`)
- book title (`String`)
- book author (`String`)
- book path (`String`)
- repeated clipping records:
  - `spineIndex` (`uint16_t` LE)
  - `startPage` (`uint16_t` LE)
  - `endPage` (`uint16_t` LE)
  - `pageCount` (`uint16_t` LE, at least `1`)
  - `startWordIndex` (`uint16_t` LE)
  - `endWordIndex` (`uint16_t` LE)
  - `wordCount` (`uint16_t` LE)
  - `paragraphIndex` (`uint16_t` LE, `UINT16_MAX` when unavailable)
  - `timestamp` (`uint32_t` LE, UTC Unix seconds when saved, 0 without a clock; older records hold seconds since boot, read as unknown)
  - versions 3-4: reader layout signature (`uint32_t` LE; font, spacing,
    viewport, and other section-layout inputs)
  - version 4: table selection (`uint16_t` LE; `UINT16_MAX` for non-table text)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - version 1: selected text (`String`; legacy files were written with a
    `512`-byte in-app limit)
  - versions 2-4: selected-text length (`uint16_t` LE) followed by that many
    UTF-8 bytes (the current in-app limit is `4096` bytes, defined by
    `CLIPPING_TEXT_MAX`)

The clipping selector has a separate navigation bound: it exposes at most
`240` visible words from at most three pages. This is a bounded in-memory
selection window for low-memory devices, not a character-count limit. The
selected text is still stored separately and is limited to `4096` UTF-8 bytes.

CrossDink uses the stored spine/page/paragraph fields as anchors, then searches
near that location for the stored clipping text after relayout. This is similar
to keeping both a DOM position and a text quote in a web app: the numeric
position gives a fast starting point, while the text makes jumps and highlights
survive font, layout, or page-count changes when possible.

Version 3 records which reader layout produced the numeric page/word anchor.
When that signature differs, CrossDink ignores the stale numeric range and
matches the saved text instead, including when both layouts happen to have the
same total page count. Legacy records without a layout signature use text
matching rather than trusting ambiguous numeric ranges. Version 4 adds the
table selection field; versions 1-3 remain readable.

Creating a clipping also appends a Kindle-style export entry to
`/My Clippings.txt` on the SD-card root. That text export can keep up to `2000`
bytes of the selected text and is append-only. Removing a clipping from the
reader deletes or rewrites only the binary clipping file; it does not remove
previous entries from `/My Clippings.txt`.

When CrossDink moves an EPUB through its built-in move-to-Read flow, it rewrites
the clipping file under the new path-derived name and removes the old one. If a
book is renamed or moved outside CrossDink, the path hash changes, so the old
clipping file may no longer be associated with the book until the file is moved
back or the clipping store is migrated.

## `epub_<hash>/links.bin`

The EPUB reader saves followed-link Back history on clean exit (Home, sleep,
or reader replacement for sync) and before a silent restart. The record is a
fixed 13 bytes: a one-byte depth (0–3), then three pairs of little-endian `u16`
spine index and `u16` page number, oldest first, unused pairs zero. Depth 0 is
empty history. A book that never followed a link has no file. A transient
footnote preview resumes at its immediate origin and omits that final entry;
earlier full-section links remain in the record. Writes are held in RAM until
the next flush point (sleep, restart, low battery; `HeldWrites`), so page turns
never write it.

On open, the reader checks the length and each spine index, then overwrites the
record with depth 0 before adopting the history. Malformed records are also
consumed. A later clean exit rewrites the current stack; a crash or reset cannot
revive history from a previous session (power loss before the next flush can).
This sidecar does not change EPUB layout cache formats. As with in-memory Back
history, changing font or layout may shift the destination page.

## EPUB `progress.bin` and `progress.bin.bak`

### Slot record (20 bytes)

The two files are fixed-size slots. Each save overwrites the slot holding the
older record in place (no truncate, no rename), so a save touches one data
sector plus the directory entry. A torn write fails the CRC and the reader uses
the other slot, which holds the previous save. Saves whose position matches the
current record are skipped.

Binary layout (little endian):

- `[0-1]` spine index (`uint16_t`)
- `[2-3]` page number (`uint16_t`, `0xFFFF` reads as page 0)
- `[4-5]` page count (`uint16_t`)
- `[6-9]` visible-text offset (`uint32_t`, `0` when absent)
- `[10]` marker `0xC5`
- `[11]` flags (`bit0=visible-text offset present`)
- `[12-15]` sequence number (`uint32_t`, compared modulo 2^32)
- `[16-19]` CRC-32 (IEEE) of bytes `0-15`

Selection: a legacy `progress.bin` wins (only older firmware writes it, and it
writes `progress.bin` last); otherwise the valid slot with the newer sequence
number. The next save targets whichever file is not a valid slot record,
`progress.bin` first, then the older slot.

Bytes `0-9` are the legacy record, so older firmware, which reads at most 10
bytes, still finds a position in `progress.bin` (at most one save stale).

### Legacy records

4 bytes (spine, page), 6 bytes (plus page count), or 10 bytes (plus visible-text
offset), written by older firmware through a temp file and a
`progress.bin` -> `progress.bin.bak` rotation. Still read; replaced on the next save.

## EPUB `progress_percent.bin`

The book's % for Home and Library, so they draw without opening the book.
Little-endian.

### Version 2

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | Magic `0x45505250` ("EPRP") |
| 4 | 1 | Version `2` |
| 5 | 2 | % in basis points (0-10000) |
| 7 | 2 | KOSync mark: the % in basis points when a KOSync check last found the server holding it, `0xFFFF` = never checked |
| 9 | 1 | Flags: bit 0 = the server was further on at that check |

The mark is held in RAM after a check and written at deep sleep, restart, OTA,
USB Drive and File Transfer, so a power cut loses it (the book then shows as not
synced until its next check). Home and Library draw the KOSync badge from it.

### Version 1

Bytes 0-6 of version 2 (CrossInk writes this one). Read as never checked.

## `stats_v5.bin`

### Version 5

`stats_v5.bin` stores per-book reading statistics for stats schema version 5.
Versioned filenames let firmware branches with different stats schemas keep
their own per-book stats files without overwriting each other. Version 5 extends
version 4 with a cached live reader book time-left estimate so Home and Reading
Stats can show the same estimate the reader last computed.

When `stats_v5.bin` is missing, CrossDink can read the previous versioned stats
filename (`stats_v4.bin` for version 5, `stats_v5.bin` after a future version 6
bump) before falling back to legacy `stats.bin` files with compatible stats
payloads. Future changes are always saved to the current versioned filename.

Binary layout:

- `[0]` version (`5`)
- `[1-2]` `sessionCount` (`uint16_t` LE)
- `[3-6]` `totalReadingSeconds` (`uint32_t` LE)
- `[7-10]` `totalPagesTurned` (`uint32_t` LE)
- `[11]` `isCompleted` (`uint8_t`)
- `[12-13]` `avgSecondsPerForwardPage` (`uint16_t` LE)
- `[14-15]` `paceSampleCount` (`uint16_t` LE)
- `[16]` flags (`bit0=startDateManual`, `bit1=finishedDateManual`)
- `[17-20]` `startDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[21-24]` `finishedDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[25-40]` `timeOfDaySeconds[4]` (`uint32_t` LE each)
- `[41-68]` `dayOfWeekSeconds[7]` (`uint32_t` LE each)
- `[69-72]` `estimatedTimeLeftSeconds` (`uint32_t` LE, `0` means unavailable)

## `section.bin`

### Version 100

The payload is unchanged. Fieldset containers now get a default 1 px solid
outline, 2 px side margins and padding (0.35em top, 0.625em bottom, 0.75em
sides); publisher CSS overrides each part, and Light mode or disabled book
styles draw no box. Complete files use byte `100`; suspended partials use
`0xEC`, and `0xED` joins the retired sentinels.
### Version 101

Complete caches use `101`; suspended caches use `0xEB`, and `0xEC`/`0xED` join
the retired sentinels (`100`/`0xEC` belong to the fieldset change). After the
render-mode byte, the header adds `u8 publisherFonts`, `u32
publisherFontSignature` (face manifest identity), `u32 publisherFontFaces`
(requested face bitmask), `u32 publisherFaceSignature` (loaded face
fingerprints and fallback results) and `u32 publisherLoadedFaces` (successfully
admitted faces). The face state is captured after page layout and patched when
the complete or partial cache is committed. Reopening restores only requested
families, previously loaded faces first, and rejects the cached layout if their
availability or fingerprints differ. A book without `@font-face` faces stores
mode 0 and lays out as with the setting off.

Text blocks add a boolean byte after their packed arena, followed by one `u32`
CSS family hash per word when present, before the ruby count. A zero family
keeps the reader font. Drop caps add a `u32` family hash before their text
length.

CSS cache version `27` adds a four-byte family hash to each style (96 bytes
total); defined bit 29 marks an explicit family. After the descendant rules,
the same CSS cache holds an 8-bit face count (maximum 32). Each face stores a
32-bit family hash, style flags (bit 0 bold, bit 1 italic), an 8-bit path length
and that many UTF-8 path bytes, relative to the EPUB root. A final `u32` points
to the face count so the font loader reads it directly. Empty manifests still
store the count and footer.

Extracted face files are `publisher-font-<index>` in the book cache folder and
are reused while their size matches the EPUB entry.

### Version 99

Scalable-font EPUB headings and whole text blocks carry a resolved point size
and line height. Each serialized `TextBlock` appends `u8 fontSize` (0 = reader
font, otherwise 8-44 pt) and `u16 lineHeight` after `directionDefined` in its
`BlockStyle` payload. The inherited Q8 font scale is layout-only and is not
serialized. Nested blocks also keep inherited CSS bold and italic styles, which
can change glyphs and wrapping. Complete files use byte `99`; suspended partials
use `0xED`, and `0xEE` joins the retired sentinels.

Version 99 also adds publisher decorations, contextual CSS selectors, preserved
whitespace and inline font sizes. TextBlock adds one-byte `wordSizesPresent` and
`initialLetterBytes` fields immediately before `textBytes`. The initial-letter
prefix (0-12 bytes) stays in the first logical word for dictionary lookup and
clippings; ordinary text rendering skips those bytes because the drop cap draws
them. When `wordSizesPresent` is set, the arena stores one point-size byte per
word after the whitespace bitset and before UTF-8 text; zero uses the block
font. The flag is zero for ordinary lines, so they need no extra per-word
storage. Sizes use the existing scalable font family and share the tallest
word's baseline and line height.

Page tags `5` and `6` are drop caps and border boxes. Drop caps contain x/y
(int16 each), source font point size (uint8, zero means the reader font), Q8
scale (uint16), style (uint8), UTF-8 byte length (uint8, 1-12), then text bytes.
Border boxes contain x/y (int16 each), width/height (uint16 each), four pairs of
width/style bytes in top/right/bottom/left order, and a shaded byte. Styles are
none/solid/double/dotted/dashed (0-4), and border widths are capped at 8 pixels.

The CSS rule cache moves to version `26`. Each
style payload adds a five-byte font-size length (float value plus unit) after
`imageWidth`, then after the four border masks a whitespace byte, eight border
width/style bytes and shaded/float-left/initial-letter bytes, before the uint32
defined flags. Defined-property bit 23 marks font size; bits 25-28 mark
whitespace, shading, float and initial-letter declarations. The fixed style
payload grows from 75 to 92 bytes. Context-rule records keep the two-string
format; the first string is the ancestor chain (`@` means no constraint), and
the second is the subject compound, optionally ending in `::first-letter`.
Older CSS caches rebuild.

On scalable fonts, headings default to 2, 1.5, 1.17, 1, 0.83, and 0.67 times the
inherited size, rounded to whole points and bounded to 8-44 pt. Enabled book
styles can override block sizes using em, rem, %, px, pt, and size keywords.
CSS 16px/12pt maps to the user's selected body size; em/% use the parent and
rem uses the HTML root. Inline span size changes and table-cell sizing remain
uniform. Light mode and bitmap fonts retain their existing sizes.
### Version 98

Version 98 places small inline images within text lines while keeping larger
images as centered blocks. Page-image records add a one-byte inline flag after
their coordinates. Full caches (byte `96`) and suspended partial caches (`0xF0`)
rebuild so existing books receive the new layout. Complete files use byte `98`;
suspended partials use `0xEE`, and `0xEF` and `0xF0` join the retired sentinels.
Version 97 and `0xEF` are skipped: another CrossDink change uses them. The CSS
rule cache moves to version `25` so `display: inline` rules retain their meaning.
Image-only paragraphs keep publisher alignment independently of the reader's
prose alignment.

### Version 96

Version 96 keeps the version 79 serialized layout. Paragraphs inherit
`text-indent` from `<html>` and `<body>`, an `<hr>` whose CSS hides every border
edge keeps its spacing but draws no rule, and `<figure>` is a block container
whose publisher alignment and margins apply once around its images. Full caches
(byte `79`) and suspended partial caches (`0xF1`) rebuild because page positions
change. Complete files use byte `96`; suspended partials use `0xF0`, and `0xF1`
joins the retired sentinels. Versions 80–95 are skipped: CrossInk uses them for
other layouts, so a later sync cannot accept one of its caches as current.

The CSS rule cache moves to version `24` (versions 19–23 occur in CrossInk and
local branch history). Each style payload appends four border edge masks after
`listStyleType`: suppressed styles, zero widths, defined styles and defined
widths (one byte each, bits 0–3 for top/right/bottom/left). Defined-property
bit 24 marks border state. The fixed style payload grows from 71 to 75 bytes.
Older CSS caches rebuild.

### Version 97

Complete caches use `97`; suspended partial caches use `0xEF` (`0xF0` and `0xF1`
are retired). Version 96 is left to the inherited-paragraph-style change. The
section header adds signed `characterSpacing` (-5..5) immediately after
`wordSpacing`. Each TextBlock adds the same signed byte immediately before
`textBytes`, so cached drawing keeps the exact spacing used for layout. Older
full and partial caches rebuild automatically.
### Version 79

Version 79 keeps the version 78 serialized layout. Korean words now wrap at
spaces by default; with hyphenation enabled, they can also split at a legal
CJK boundary at a line end without a visible hyphen. Justification stretches
word spaces only. Full caches (byte `79`) and suspended partial caches
(`0xF1`) both rebuild because earlier page positions are no longer valid.
Partial sentinels `0xF2`–`0xFE` are all retired (v75 already used `0xF4`), and
`Section.cpp` rejects any reuse at compile time.

### Version 78

Version 78 changed layout for inline CSS padding. Full and suspended partial
section caches rebuild together; suspended partials use sentinel `0xF2`.

### Version 77

Version 77 keeps the serialized layout unchanged. It was bumped because ordered
lists now number their items, `list-style-type: none` suppresses markers, and
`<ul>`/`<ol>` margins and padding contribute to child insets. Complete files use
byte `77`; suspended partials use the previously unused sentinel `0xF3`.
The related CSS rule cache uses version `18`; version `17` already occurs in
local branch history.

### Version 75

Version 75 keeps the serialized layout unchanged but excludes EPUB elements with
the HTML `hidden` attribute. Complete files use byte `75`; suspended partials use
`0xF4`. Both older full and partial layouts rebuild automatically.
Versions 67–74 and partial sentinel 0xF5 already occur in other local branch
history; using fresh identifiers avoids accepting those experimental caches.

### Version 66

Version 66 keeps the version 63 serialized layout unchanged. It was bumped
because internal EPUB links now preserve CSS superscript and subscript styles,
changing their cached word-style flags and page layout. Complete files use
version byte `66`, and suspended partials use sentinel byte `0xF6`.

The stable v1.5.1 release retains these identifiers from RC6. Do not normalize
published RC versions to the previous stable version plus one: v1.5.0 used
`60` / `0xF9`, and RC4 already shipped `61` / `0xF8` with older layout output.
Reusing those identifiers could accept stale RC caches as current. Per-book
reader settings likewise retain version `9` and their version 7/8 migrations.

### Version 62

Version 62 stores one compact source-whitespace bit per word in serialized text
blocks. Touch reader previews use it to reflow words with the selected font
without inferring spaces from device-specific pixel advances. Full and
suspended section caches rebuild together; complete files use version byte
`62`, and suspended partials use sentinel byte `0xF8`.

### Version 61

Version 61 was an earlier v1.5.1 release-candidate cache update. It stores `protectedImageUnits`
(`uint32_t` LE) after `pageCount`, so image-heavy sections estimate their
remaining non-image pages accurately. It also updates table fragments and
geometry, oversized-word wrapping, inline-image margins, and ruby continuation
layout. Full and suspended section caches rebuild together; complete files use
version byte `61`, and suspended partials use sentinel byte `0xF8`.

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 59 adds a compact page-start visible-text-offset lookup table. The
offset is a Unicode codepoint coordinate in the spine XHTML, so reader progress
and KOReader sync can return to the same content after a font, orientation, or
indexing-method change instead of relying on a page percentage. Suspended
incremental caches store the same table for their readable prefix; a target
beyond that prefix must continue indexing before it can be resolved.

Version 57 is binary-identical to version 56. The version was bumped because
word-gap suppression now applies only to tokens glued together in the source.
Older caches could collapse explicit spaces between Hangul words, so full and
suspended partial section caches rebuild together. Version 58 recalculates
Focus Reading split-run offsets with the renderer's combined advance and
kerning rounding, so old cached page positions rebuild.

Version 56 changes `<br>` layout: a line break after text no longer reapplies
the containing block's top or bottom spacing, while an empty `<br>` block keeps
the existing scene-break gap. Full and suspended partial section caches rebuild
together. Version 55 assigns compact IDs to internal EPUB links. The ID is
stored in the existing per-word flags byte and in each page's footnote entry so
touch devices can map tapped text to the existing fragment-navigation path
without retaining another per-word data structure. Version 54 adds compact
ruby-text annotations to serialized text blocks. Only words that begin a ruby
group store annotation text; continuation words use a dedicated style bit. This
keeps books without ruby markup unchanged apart from the cache version while
avoiding an empty string allocation for every word.
Version 53 stores each image's EPUB-internal source path so section indexing can
read only its header and defer full extraction until the page is shown. Version
52 keeps Guide Dots centered when extra word spacing is enabled. Version 51
preserves continuation state for oversized CJK word fragments. Version 50
paginates chapter-heading image runs within the reader viewport so they do not
overflow into the reserved status-bar area. Version 49 stores Focus Reading
split-run offsets in visual order so RTL word prefixes render on the right.
Version 48 changed Arabic contextual shaping and text measurement, so cached
word positions from version 47 no longer match what `drawText` renders.

Version 48 makes the EPUB word-spacing level widen the natural inter-word gap
(each level adds 10 pixels), which changes laid-out word positions, so
older sections must rebuild. Version 46 added the EPUB word-spacing level to the
cache-busting header. It retains the flat `TextBlock` arena and chapter-opener
anchor behavior introduced in version 45. It includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Focus Reading, Guide Dots, word spacing,
  and EPUB render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- visible-text-offset LUT used to resolve page positions across reflow and sync
- optional per-word Focus Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds, layout-inserted hyphens,
  and internal-link IDs
- reading-aid layout that stores Focus Reading and Guide Dots as per-word metadata instead of temporary layout words
- publisher CSS page-break handling and adjusted justification spacing baked into page layout
- table fragments
- per-page footnote entries
- per-page publisher page markers
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage: per-word arrays plus one shared NUL-terminated
  text blob, replacing length-prefixed word strings and parallel vectors. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 79
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageTableFragment = 3,
    TAG_PageHorizontalRule = 4
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
    u8 fontSize;
    u16 lineHeight;
};

struct TextBlock {
    u16 wordCount;
    u8 hasFocus;
    u8 hasGuideDots;
    u8 hasWordFlags;
    s8 characterSpacing [[comment("-5..5, half-pixel steps between letters")]];
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasFocus != 0) {
            u16 wordFocusSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasFocus != 0) {
            u8 wordFocusBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        if (hasWordFlags != 0) {
            u8 wordFlags[wordCount] [[comment("bit 0 = black background, bit 1 = layout-inserted trailing hyphen")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String sourcePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount];
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageTableFragment) {
        PageTableFragment tableFragment [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
    u8 linkId;
};

struct PublisherPageMarker {
    s16 yPos;
    char label[16];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    u8 publisherPageMarkerCount;
    PublisherPageMarker publisherPageMarkers[publisherPageMarkerCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    bool forceParagraphIndents;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool focusReadingEnabled;
    bool guideReadingEnabled;
    u8 wordSpacing;
    s8 characterSpacing;
    u8 renderMode; // 0 = CrossDink Default, 1 = Balanced, 2 = Light

    u16 pageCount;
    u32 protectedImageUnits;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## Optimizer image transport (PXC2 and COIX)

Optimizer images are optional EPUB sidecars. Original JPEG/PNG assets remain the
compatibility fallback. `META-INF/crossink/optimizer-v1.json`,
`META-INF/crossink/optimizer-images-v1.idx`, and all
`META-INF/crossink/pxc/*.pxc2` entries must use ZIP STORE. PXC2 has its own block
compression; the firmware rejects ZIP-deflated PXC2 before starting an inflater.
The manifest keeps `format: "crossink-optimizer"` and `version: 1`. Each image has
`href`, `pxc`, `width`, `height`, `pxcFormat: "pxc2"`, `pxcBytes` (complete transport
size), and `pixelCrc32`. Missing `pxcFormat` means legacy raw PXC1.

All integers below are unsigned little endian. Fields are serialized explicitly,
not by writing native C++ structures. CRCs use standard IEEE CRC32 (zlib).

### PXC2

The 32-byte header is:

| Offset | Bytes | Field                                                              |
| ------ | ----- | ------------------------------------------------------------------ |
| 0      | 4     | `PXC2`                                                             |
| 4      | 1     | Version 2                                                          |
| 5      | 1     | Pixel format 1: four 2-bit pixels per byte, most significant first |
| 6      | 2     | Header bytes: 32                                                   |
| 8      | 2     | Width                                                              |
| 10     | 2     | Height                                                             |
| 12     | 2     | Row bytes: `(width + 3) / 4`                                       |
| 14     | 2     | Block bytes: 2048                                                  |
| 16     | 2     | Block count: ceiling of raw bytes / 2048                           |
| 18     | 2     | Flags: zero                                                        |
| 20     | 4     | Raw pixel bytes                                                    |
| 24     | 4     | Raw pixel CRC32                                                    |
| 28     | 4     | Complete PXC2 file bytes                                           |

Dimensions are 1–1024, raw data is at most 128 KiB, and there are at most 64
blocks. Each block starts with a 12-byte header: codec (`u8`, 0 RAW, 1 raw DEFLATE,
2 PackBits), zero flags (`u8`), raw length (`u16`), encoded length (`u16`), sequence
(`u16`, starting at zero), decoded-block CRC32 (`u32`). Lengths are 1–2048. All
non-final blocks have 2048 raw bytes. Compressed blocks must be smaller than raw;
otherwise producers use RAW. Every block is independent. PackBits controls 0–127
copy the next control+1 bytes; 129–255 repeat the next byte 257-control times;
128 is rejected. Raw DEFLATE must terminate exactly, with no extra input/output.
Python uses zlib level 8 and `wbits=-15`; the browser uses PackBits/RAW.

The reader reuses a fallibly allocated session workspace (5928 bytes on the
64-bit host; firmware size is compile-time limited below 6500), verifies all
lengths/CRCs and resizes rows into `img_*.pxc.optimizer.tmp`. It checks size and
syncs/closes before publishing. On filesystems that cannot rename over an existing
file, `.optimizer.previous` retains the previous cache until publication succeeds;
failed rollback leaves that backup available for recovery on the next attempt. PXC2 never needs a full raw source temporary file.
The local raw PXC layout remains two `u16` dimensions plus packed rows. No section
cache version change is required. Hardware heap and visual results remain device
acceptance checks, not implied by host workspace accounting.

### COIX version 1

The local index is `/.crossdink/epub_<hash>/optimizer-images.idx`. Its header is:

| Offset | Bytes | Field                            |
| ------ | ----- | -------------------------------- |
| 0      | 4     | `COIX`                           |
| 4      | 2     | Version 1                        |
| 6      | 2     | Header bytes: 32                 |
| 8      | 2     | Record bytes: 208                |
| 10     | 2     | Record count: 0–256              |
| 12     | 4     | Flags/reserved: zero             |
| 16     | 4     | Manifest central-directory CRC32 |
| 20     | 4     | Manifest uncompressed bytes      |
| 24     | 4     | CRC32 of all records             |
| 28     | 4     | CRC32 of header bytes 0–27       |

Each 208-byte record contains NUL-terminated `href[129]` (offset 0),
`pxcHref[65]` (129), width `u16` (194), height `u16` (196), format `u8` (198,
1 legacy or 2 PXC2), zero flags `u8` (199), complete sidecar bytes `u32` (200),
and pixel CRC32 `u32` (204). Paths are relative, bounded UTF-8; traversal,
backslashes, control characters, colons and percent escapes are rejected.
Sidecars must be beneath `META-INF/crossink/pxc/`. Duplicate image hrefs are invalid;
producers keep one optional sidecar per href and omit unsupported/over-limit
entries. Firmware can resize that sidecar for other layouts.

Reader setup validates a packaged index and copies it through a temporary local
file, or builds it from a legacy manifest while the framebuffer is loaned. Generic
`Epub::load()` does not build indexes. A lookup scans records on SD and retains
only one last hit. Manifest identity changes invalidate the local index. Per-page
preflight reuses local pixels first, then materializes just that page's sidecars,
then extracts original assets as fallback. Legacy ZIP inflation also runs under
the framebuffer loan. Sleep-page generation uses the same preflight. Temporary
index files are cleaned during setup and per-image temporaries during preflight.

Hardware acceptance: clear only the test book's cache, open legacy/PXC2 variants
with SD font and AA, visit/revisit image pages and sleep, compare portrait and
landscape output, and record internal free/largest heap blocks and low-water
marks. Repeat corrupt sidecars, full/read-only SD, interrupted writes and book
replacement at the same path on Sticky (SPI SD), X4 Pro and X4 Classic (SDMMC).

### CSS rules cache revision 18

Revision 18 adds the serialized `list-style-type` property used to number
ordered lists and suppress list markers. It also includes the PSRAM streamed
stylesheet path introduced in revision 16, which admits sources up to 512 KiB
on PSRAM readers (every CrossDink device); builds without a PSRAM heap,
including the simulator, keep the 128 KiB limit. Existing rule-count and
internal-memory guards still apply. Rebuilding an invalid CSS cache also
invalidates section caches through the existing EPUB-load path, so books that
previously cached zero rules can restore hidden content and layout rules.

## S3 scalable reader fonts

Static TTF support uses font-content and backend identities to invalidate
affected EPUB layouts. Section-cache serialization is unchanged. Existing
`.cpfont` files remain supported; see [scalable fonts](scalable-fonts.md) for
limits and lifecycle.

## `/.crossdink/font-catalog.bin`

### Versions 1–2

Disposable font metadata cache, shared by reader, settings and web font controls.
The 24-byte little-endian header contains magic `0x46434931`, version, a 64-bit
inventory fingerprint, family count (maximum 128), and scalable-font build mode.
It is followed by 152-byte family summaries: a NUL-terminated 128-byte name,
32-bit detail offset/byte count/FNV-1a hash, 16-bit file count, minimum/maximum
point sizes, a scalable flag, three reserved zero bytes, and a 32-bit FNV-1a
checksum of the preceding summary bytes. Detail blocks follow the summaries.
Each detail is three bytes (point size, style, path length) followed by the
UTF-8 path bytes. Paths are at most 255 bytes; families contain at most 256
files. The whole cache is capped at 2 MiB.

Names/range labels load without retaining file paths. Only a requested family
hydrates its detail vector. Missing, incompatible or malformed summaries rebuild
the catalog by scanning names first and writing one family's paths at a time;
invalid detail blocks invalidate the cache for the next request.
Temporary memory failures preserve the font selection and cache for retry.
Writers finish and sync `font-catalog.tmp` before replacing the cache. An
interrupted replacement is safe because no user data is stored here.

On first font metadata access after boot or explicit invalidation, an inventory
walk checks names, directory kinds and file lengths in both font roots and their
immediate subdirectories. It reads no font contents on an index hit, and is not
repeated when catalog RAM is released and reloaded during the same session. This
detects externally added/removed files and changed lengths after restarting;
same-length content-only edits outside the firmware are not detectable by that
check. Firmware upload/delete/move/download paths invalidate explicitly,
including failed dedicated font uploads; USB Drive invalidates before handing
the card to the host. Manage Fonts performs a full rescan; alternatively remove
this cache to force reinspection after external same-length font changes.

EPUB layout cache versions and identities are unchanged by this catalog.

## OPDS server list

`/.crossdink/opds.json` holds `servers`, an array of objects with `name`, `url`, `username`, `password_obf`, `filenameFormat` (`author_title`, `title_author` or `server_filename`) and an optional `downloadFolder`. A missing or empty `downloadFolder` uses the global `opdsDownloadFolder` setting; `/` is the SD root; anything else is an absolute folder path of at most 63 bytes, normalized on load. An invalid value is logged and treated as empty. Older files need no migration.
Version 2 rescans `.ttf` and `.otf` files so static TrueType and OpenType CFF
faces rejected by earlier firmware enter the catalog. The binary layout is unchanged.
