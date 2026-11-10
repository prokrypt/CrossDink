---
title: SD Card Themes
nav_order: 4.5
---

# SD Card Themes

A theme pack is a folder on the SD card that restyles a built-in theme: its
sizes and shapes, its icons, and optionally the Home screen and your own
shortcut pages. Packs are data only. They cannot run code or change how the
display refreshes, so a pack cannot add flashes or drain the battery.

Two sample packs live in [`docs/themes/`](./themes/): `bold` (big rows, square
corners, thick icons, a three-column Home grid) and `shelf` (rounded outlines,
a cover grid, a banner image and a user page).

## Install And Pick

1. Copy the pack folder to `/.themes/<name>/` or `/themes/<name>/` on the SD
   card. Both roots are scanned; when a name is in both, `/.themes/` wins.
2. Open **Settings > Display > Theme pack** and pick the folder name. The row
   appears once a pack is on the card. **None** turns packs off, and picking a
   built-in **UI Theme** also turns the pack off.
3. Picking the same pack again re-reads it, so an edited pack shows without a
   restart.

The pack is read once, when picked and at boot. Drawing never reads the card.

In the simulator the SD card root is `./fs_/`, so a pack goes in
`./fs_/themes/<name>/`.

## When Something Is Wrong

- A missing or broken `theme.json` shows the toast **Theme pack not loaded**
  and keeps the built-in theme. The pack name stays picked, so a fixed file
  loads on the next pick or boot.
- A broken `home.json` keeps the pack's look and the base theme's Home, with
  the same toast.
- Unknown keys, blocks and entries are skipped. Numbers outside their range
  are clamped. Each problem is logged on serial as a `[THM]` line naming the
  file and the reason.
- Two crash reboots in a row with a pack set turn the pack off and show
  **Theme pack off after crashes**.
- Removing the SD card always gives the built-in theme.

## Folder Layout

    themes/
    └── shelf/
        ├── theme.json      required
        ├── home.json       optional: the Home screen
        ├── shelf.bmp       images and icons: any file name in the folder
        └── pages/
            └── Reading.json  optional: a user page, opened by page:Reading

File names inside JSON are relative to the pack folder; `..`, `\` and leading
`/` are refused. Each JSON file can be up to 16 KB.

## theme.json

```json
{
  "name": "Bold",
  "base": "lyra",
  "metrics": { "listRowHeight": 48, "listSelectionStyle": 0, "listTitleBold": true },
  "icons": {
    "settings": "icons/settings32.bmp",
    "folder": ["icons/folder24.bmp", "icons/folder32.bmp"]
  }
}
```

- `name`: free text for people reading the file. The picker shows the folder
  name.
- `base` (required): the built-in theme to start from. One of `lyra`,
  `lyra-3-covers`, `lyra-carousel`, `minimal`, `dashboard`, `cover-grid`,
  `classic`, `roundedraff`. Touch devices do not offer `classic` and
  `roundedraff`, so a pack with those bases does not load there.
- `metrics`: any of the keys below. Keys you leave out keep the base theme's
  value. Sizes are in pixels.
- `icons`: replaces built-in icons by name. Each value is one file or a list
  of a 24 px and a 32 px file. An icon is a 1-bit BMP, square, 24x24 or 32x32;
  black is ink, white is transparent. A size you leave out keeps the built-in
  icon at that size. Names: `folder`, `text`, `image`, `book`, `bookmark`,
  `file`, `recent`, `settings`, `transfer`, `library`, `opds`, `wifi`,
  `hotspot`, `chart`, `flask`.

### Metrics

Touch-sized rows (menu rows, tab bar, keyboard keys) keep minimums near the
44 px touch target.

| Key | Range | Notes |
| --- | --- | --- |
| `batteryBarHeight` | 20 to 80 |  |
| `headerHeight` | 30 to 140 |  |
| `verticalSpacing` | 0 to 40 |  |
| `previewPadding` | 0 to 40 |  |
| `previewHeightPercent` | 10 to 60 |  |
| `contentSidePadding` | 0 to 60 |  |
| `listRowHeight` | 24 to 80 |  |
| `listWithSubtitleRowHeight` | 40 to 120 |  |
| `listRowGap` | 0 to 20 |  |
| `listRowRadius` | 0 to 30 |  |
| `listInset` | 0 to 40 |  |
| `listSidePadding` | 0 to 40 |  |
| `listSelectionStyle` | 0 to 5 | 0 invert fill, 1 light pill, 2 underline, 3 triangle, 4 outline, 5 gray outline |
| `listScrollWidth` | 1 to 10 |  |
| `listScrollSide` | 0 to 1 | 0 right edge, 1 left edge |
| `listTitleBold` | true or false |  |
| `headerSidePadding` | 0 to 40 |  |
| `headerUnderlineSize` | 0 to 6 |  |
| `headerTitleAlign` | 0 to 2 | 0 left, 1 center, 2 right |
| `headerBatterySide` | 0 to 1 |  |
| `headerBatteryDetached` | true or false |  |
| `menuRowHeight` | 42 to 100 |  |
| `menuSpacing` | 0 to 30 |  |
| `tabSpacing` | 0 to 30 |  |
| `tabBarHeight` | 40 to 80 |  |
| `tabBarAppearance` | 0 to 1 | 0 pill, 1 bordered text |
| `scrollBarWidth` | 1 to 10 |  |
| `scrollBarRightOffset` | 0 to 20 |  |
| `homeTopPadding` | 20 to 120 |  |
| `homeMenuTopOffset` | 0 to 60 |  |
| `progressBarHeight` | 4 to 30 |  |
| `keyboardKeyHeight` | 44 to 80 |  |
| `keyboardKeySpacing` | 0 to 16 |  |
| `keyboardCenteredText` | true or false |  |
| `keyboardVerticalOffset` | -30 to 30 |  |
| `keyboardTextFieldWidthPercent` | 50 to 100 |  |
| `keyboardWidthPercent` | 70 to 100 |  |
| `popupTopOffsetRatio` | 0 to 0.4 | share of the screen height |
| `popupMarginX` | 0 to 40 |  |
| `popupMarginY` | 0 to 40 |  |
| `popupFrameThickness` | 0 to 6 |  |
| `popupCornerRadius` | 0 to 30 |  |
| `popupTextBold` | true or false |  |
| `popupTextInverted` | true or false |  |
| `popupTextBaselineOffsetY` | -10 to 10 |  |
| `popupProgressBarHeight` | 1 to 12 |  |
| `popupProgressDrawOutline` | true or false |  |
| `popupProgressClampPercent` | true or false |  |
| `popupProgressFillInverted` | true or false |  |
| `popupProgressOutlineInverted` | true or false |  |
| `optionPopupItemSpacing` | 0 to 24 |  |
| `optionPopupInnerPadding` | 0 to 40 |  |
| `optionPopupSelectionHPadding` | 0 to 40 |  |
| `optionPopupSelectionVPadding` | 0 to 24 |  |
| `optionPopupTitleGap` | 0 to 32 |  |
| `optionPopupUseSmallFont` | true or false |  |
| `optionPopupOptionFontBold` | true or false |  |
| `optionPopupSelectionRadius` | 0 to 40 |  |
| `optionPopupSelectionLight` | true or false |  |
| `optionPopupDrawAllRows` | true or false |  |
| `optionPopupDialogSideMargin` | 0 to 60 |  |
| `optionPopupTitleSeparator` | true or false |  |
| `textFieldHorizontalPadding` | 0 to 20 |  |
| `textFieldNormalThickness` | 1 to 4 |  |
| `textFieldCursorThickness` | 1 to 6 |  |
| `textFieldLineEndOffset` | -10 to 10 |  |

## home.json

```json
{
  "blocks": [
    {"type": "header"},
    {"type": "covers", "style": "row", "count": 3, "height": "45%"},
    {"type": "stats", "items": ["streak", "today", "book_progress"]},
    {"type": "menu", "style": "grid", "columns": 2,
     "items": ["continue", "library", "files", "folder:/Comics", "page:Comics", "settings"]}
  ]
}
```

Blocks stack from top to bottom, at most 15. Keys a block can have: `type`,
`style`, `count`, `columns`, `height`, `items`, `text`, `file`.

| Type | What it shows | Keys |
| --- | --- | --- |
| `header` | The Home status header (battery, clock) | |
| `covers` | Recent books | `style`: `single` (a book card), `row` (default) or `grid`; `count` 1 to 7 (default 3, grid 6); `columns` 1 to 4 for grid (default 3) |
| `stats` | Up to 4 reading stats | `items`: `streak`, `today`, `total_time`, `books_read`, `book_progress`, `book_time` |
| `menu` | Home entries | `style`: `list` (default) or `grid`; `columns` 1 to 4 for grid (default 2); `items`: entries, below |
| `clock` | Time and date | |
| `text` | Your own line, 1 to 63 bytes, at most 4 text blocks | `text` |
| `image` | A 1-bit BMP from the pack folder, at most 4 | `file` |
| `spacer` | Empty space | |

Covers blocks take books in recent order, 7 books across all of them.

**Height:** `"N%"` of the Home area, or `"flex"` for a share of the space
left. By default `header`, `stats`, `clock`, `text` and `image` take their
natural height and the other blocks share the rest. Percentages that add up
to more than 100 are scaled down.

**Images:** 1-bit BMP, 1 to 800 px a side, 96 KB for all of a pack's images
together. They are read once with the pack and kept in memory.

### Entries

| Entry | Opens |
| --- | --- |
| `continue` | The last book |
| `library` | Library |
| `files` | File browser |
| `opds` | OPDS catalogs |
| `transfer` | File transfer |
| `settings` | Settings |
| `stats` | Reading stats |
| `saved` | Saved items |
| `goodies` | Goodies (debug builds only) |
| `folder:/path` | That SD card folder in the file browser |
| `book:/path/book.epub` | That book |
| `page:<id>` | The user page `pages/<id>.json`, labelled `<id>` |

Built-in entries keep their translated names; a pack restyles them only
through its icons (`continue` and `book:` use `book`, `files`, `folder:` and
`page:` use `folder`, `stats` uses `chart`, `saved` uses `bookmark`, `goodies`
uses `flask`). Paths are absolute and up to 127 bytes. At most 23 entries in
all. A layout without `settings` gets it appended to its last menu, so
Settings is always reachable.

On button devices, focus moves through the blocks in order, then through the
items in each block left to right.

## User Pages

`pages/<id>.json` is a page of entries, opened from a `page:<id>` entry on
Home or on another page. It is read when opened, never at boot. Back always
returns.

```json
{ "title": "Reading", "style": "grid", "columns": 2,
  "items": ["folder:/Books", "saved", "stats", "page:Comics"] }
```

- `title`: 1 to 63 bytes; the id when left out.
- `style`, `columns`, `items`: as in a `menu` block.
- The id is a file name without `.json`: no `/`, `\`, or leading `.`.
