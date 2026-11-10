#pragma once

#include <ArduinoJson.h>

#include <cstddef>
#include <cstdint>

// A theme pack's Home layout: home.json beside theme.json. Data only: blocks from a
// fixed set, stacked top to bottom by PackHomeUi, and Home entries from a fixed
// allowlist. Read once with the pack (ThemePack::sync), never per frame.
//
// { "blocks": [
//     {"type": "header"},
//     {"type": "covers", "style": "row", "count": 3, "height": "45%"},
//     {"type": "stats", "items": ["streak", "today", "book_progress"]},
//     {"type": "menu", "style": "grid", "columns": 2,
//      "items": ["continue", "library", "files", "folder:/Comics", "book:/Books/a.epub", "page:Comics",
//                "settings"]},
//     {"type": "clock"}, {"type": "text", "text": "Hello"}, {"type": "image", "file": "art.bmp"},
//     {"type": "spacer"} ] }
//
// - header: the Home status header (battery, clock). covers: recent books, style
//   single (a book card), row or grid (columns 1-4), count 1-7; covers blocks take
//   books in recent order, 7 in all. stats: up to 4 of streak, today, total_time,
//   books_read, book_progress, book_time. menu: style list or grid (columns 1-4).
//   clock: time and date. text: the pack's own line. image: a 1-bit BMP in the pack
//   folder (ThemePack decodes it). spacer: empty.
// - Entries: continue, library, files, opds, transfer, settings, stats, saved,
//   goodies (debug builds), folder:<path>, book:<path>, page:<id>. Built-in entries keep
//   their translated labels; a pack restyles them only through its icons. A layout
//   without settings gets it appended, so Settings is always reachable.
// - page:<id> opens the user page pages/<id>.json in the pack folder, labelled <id>:
//     { "title": "Comics", "style": "grid", "columns": 3,
//       "items": ["folder:/Comics", "book:/Comics/a.cbz", "page:Manga", "library"] }
//   title (default <id>), style list or grid (columns 1-4), items as in a menu block.
//   Read when opened, never at boot; Back always returns.
// - height: "N%" of the Home area or "flex" (a share of what is left). By default
//   header, stats, clock, text and image take their natural height, the rest share.
// - Unknown types, keys and entries are logged and skipped.
namespace ThemePackHome {

enum class BlockType : uint8_t { Header, Covers, Stats, Menu, Clock, Text, Image, Spacer };
enum class Style : uint8_t { Single, Row, Grid, List };
enum class Stat : uint8_t { Streak, Today, TotalTime, BooksRead, BookProgress, BookTime };
enum class Action : uint8_t {
  Continue,
  Library,
  Files,
  Opds,
  Transfer,
  Settings,
  Stats,
  Saved,
  Goodies,
  Folder,
  Book,
  Page
};

constexpr uint8_t kMaxBlocks = 16;
constexpr uint8_t kMaxEntries = 24;
constexpr uint8_t kMaxStats = 4;
constexpr uint8_t kMaxTexts = 4;
constexpr uint8_t kMaxImages = 4;
constexpr uint8_t kMaxBooks = 7;  // HomeCoverCache::MAX_COVERS
constexpr uint8_t kMaxColumns = 4;
constexpr uint8_t kFlex = 255;  // Block::height: a share of the space left
constexpr size_t kPathBytes = 128;
constexpr size_t kTextBytes = 64;

struct Block {
  BlockType type = BlockType::Spacer;
  Style style = Style::List;
  uint8_t count = 0;    // covers: books; stats: items; menu: entries from `first`
  uint8_t columns = 1;  // grid styles
  uint8_t first = 0;    // menu: first entry; text: text index; image: image index
  uint8_t height = 0;   // 0: default; 1-100: percent; kFlex
  Stat stats[kMaxStats] = {};
};

struct Entry {
  Action action = Action::Settings;
  char path[kPathBytes] = {};  // folder: and book: card paths; page: ids
};

struct Layout {
  Block blocks[kMaxBlocks];
  Entry entries[kMaxEntries];
  char texts[kMaxTexts][kTextBytes] = {};
  char images[kMaxImages][kTextBytes] = {};  // file names inside the pack folder
  uint8_t blockCount = 0;
  uint8_t entryCount = 0;
  uint8_t textCount = 0;
  uint8_t imageCount = 0;
  uint8_t bookCount = 0;  // books across all covers blocks
};

// Fills out from home.json's root; file names the file in log lines. False (logged)
// when nothing usable is left: Home then stays the base theme's.
bool parse(JsonObjectConst root, const char* file, Layout& out);
// A user page (pages/<id>.json): one menu block, its title in texts[0] (id when the
// file has none). False (logged) when it has no usable entry.
bool parsePage(JsonObjectConst root, const char* file, const char* id, Layout& out);

}  // namespace ThemePackHome
