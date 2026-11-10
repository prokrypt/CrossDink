#include "ThemePackHome.h"

#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace ThemePackHome {
namespace {

// Indexed by the enums in the header.
constexpr const char* kTypeNames[] = {"header", "covers", "stats", "menu", "clock", "text", "image", "spacer"};
constexpr const char* kStatNames[] = {"streak", "today", "total_time", "books_read", "book_progress", "book_time"};
constexpr const char* kActionNames[] = {"continue", "library", "files", "opds",   "transfer",
                                        "settings", "stats",   "saved", "goodies"};
constexpr const char* kCoverStyles[] = {"single", "row", "grid"};  // Style::Single, Row, Grid
constexpr const char* kBlockKeys[] = {"type", "style", "count", "columns", "height", "items", "text", "file"};
static_assert(std::size(kTypeNames) == static_cast<size_t>(BlockType::Spacer) + 1, "one name per block type");
static_assert(std::size(kStatNames) == static_cast<size_t>(Stat::BookTime) + 1, "one name per stat");
static_assert(std::size(kActionNames) == static_cast<size_t>(Action::Goodies) + 1, "one name per built-in entry");

template <size_t N>
int indexOf(const char* const (&names)[N], const char* key) {
  for (size_t i = 0; i < N; ++i) {
    if (std::strcmp(names[i], key) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool copyText(char* dst, const size_t size, const char* src) {
  const size_t len = std::strlen(src);
  if (len == 0 || len >= size) return false;
  std::memcpy(dst, src, len + 1);
  return true;
}

// An absolute card path for folder: and book: entries.
bool validCardPath(const char* path) { return path[0] == '/' && !std::strstr(path, "..") && !std::strchr(path, '\\'); }

// A page id: a file name in the pack's pages folder, without .json.
bool validPageId(const char* id) {
  const size_t len = std::strlen(id);
  return len > 0 && len < kTextBytes && id[0] != '.' && !std::strchr(id, '/') && !std::strchr(id, '\\');
}

// A file inside the pack folder.
bool validPackFile(const char* path) {
  return path[0] != '\0' && path[0] != '/' && !std::strstr(path, "..") && !std::strchr(path, '\\');
}

uint8_t parseHeight(const JsonVariantConst value, const char* file, const int block) {
  if (value.isNull()) return 0;
  const char* text = value.is<const char*>() ? value.as<const char*>() : "";
  if (std::strcmp(text, "flex") == 0) return kFlex;
  char* end = nullptr;
  const long percent = std::strtol(text, &end, 10);
  if (end != text && std::strcmp(end, "%") == 0 && percent > 0) return static_cast<uint8_t>(std::min(percent, 100L));
  LOG_ERR("THM", "%s: block %d: height must be \"N%%\" or \"flex\", ignored", file, block);
  return 0;
}

uint8_t parseCount(const JsonVariantConst value, const int fallback, const int max) {
  return static_cast<uint8_t>(std::clamp(value.is<int>() ? value.as<int>() : fallback, 1, max));
}

void parseEntry(const char* item, const char* file, const int block, Layout& out) {
  // One entry stays free for the Settings entry ensureSettings() may add.
  if (out.entryCount >= kMaxEntries - 1) {
    LOG_ERR("THM", "%s: block %d: more than %d entries, %s ignored", file, block, kMaxEntries - 1, item);
    return;
  }
  Entry entry;
  const bool folder = std::strncmp(item, "folder:", 7) == 0;
  if (std::strncmp(item, "page:", 5) == 0) {
    if (!validPageId(item + 5)) {
      LOG_ERR("THM", "%s: block %d: %s needs a file name under %u bytes, ignored", file, block, item,
              static_cast<unsigned>(kTextBytes));
      return;
    }
    copyText(entry.path, sizeof(entry.path), item + 5);
    entry.action = Action::Page;
  } else if (folder || std::strncmp(item, "book:", 5) == 0) {
    const char* path = item + (folder ? 7 : 5);
    if (!validCardPath(path) || !copyText(entry.path, sizeof(entry.path), path)) {
      LOG_ERR("THM", "%s: block %d: %s needs an absolute card path under %u bytes, ignored", file, block, item,
              static_cast<unsigned>(sizeof(entry.path)));
      return;
    }
    entry.action = folder ? Action::Folder : Action::Book;
  } else {
    const int action = indexOf(kActionNames, item);
    if (action < 0) {
      LOG_ERR("THM", "%s: block %d: entry %s unknown, ignored", file, block, item);
      return;
    }
    entry.action = static_cast<Action>(action);
#if !CROSSDINK_GOODIES
    if (entry.action == Action::Goodies) {
      LOG_ERR("THM", "%s: block %d: goodies is only in debug builds, ignored", file, block);
      return;
    }
#endif
  }
  out.entries[out.entryCount++] = entry;
}

// style list or grid, columns, items: a menu block's, or a user page's.
void parseMenu(const JsonObjectConst obj, const char* file, const int index, Block& block, Layout& out) {
  const char* style = obj["style"] | "";
  block.type = BlockType::Menu;
  block.style = std::strcmp(style, "grid") == 0 ? Style::Grid : Style::List;
  if (style[0] != '\0' && std::strcmp(style, "grid") && std::strcmp(style, "list")) {
    LOG_ERR("THM", "%s: block %d: menu style %s unknown, using list", file, index, style);
  }
  block.columns = block.style == Style::Grid ? parseCount(obj["columns"], 2, kMaxColumns) : 1;
  block.first = out.entryCount;
  for (const JsonVariantConst item : obj["items"].as<JsonArrayConst>()) parseEntry(item | "", file, index, out);
  block.count = static_cast<uint8_t>(out.entryCount - block.first);
}

void parseBlock(const JsonObjectConst obj, const char* file, const int index, Layout& out) {
  const char* typeName = obj["type"] | "";
  const int type = indexOf(kTypeNames, typeName);
  if (type < 0) {
    LOG_ERR("THM", "%s: block %d: type \"%s\" unknown, ignored", file, index, typeName);
    return;
  }
  for (const JsonPairConst kv : obj) {
    if (indexOf(kBlockKeys, kv.key().c_str()) < 0) {
      LOG_ERR("THM", "%s: block %d: key %s not supported, ignored", file, index, kv.key().c_str());
    }
  }
  Block block;
  block.type = static_cast<BlockType>(type);
  block.height = parseHeight(obj["height"], file, index);
  const char* style = obj["style"] | "";
  switch (block.type) {
    case BlockType::Covers: {
      const int coverStyle = style[0] == '\0' ? static_cast<int>(Style::Row) : indexOf(kCoverStyles, style);
      if (coverStyle < 0) LOG_ERR("THM", "%s: block %d: covers style %s unknown, using row", file, index, style);
      block.style = coverStyle < 0 ? Style::Row : static_cast<Style>(coverStyle);
      const int left = kMaxBooks - out.bookCount;
      if (left <= 0) {
        LOG_ERR("THM", "%s: block %d: all %d books already shown, ignored", file, index, kMaxBooks);
        return;
      }
      const int wanted =
          block.style == Style::Single ? 1 : parseCount(obj["count"], block.style == Style::Grid ? 6 : 3, kMaxBooks);
      block.count = static_cast<uint8_t>(std::min(wanted, left));
      if (block.count < wanted)
        LOG_ERR("THM", "%s: block %d: count cut to %d (%d books in all)", file, index, left, kMaxBooks);
      block.columns = block.style == Style::Grid  ? parseCount(obj["columns"], 3, kMaxColumns)
                      : block.style == Style::Row ? block.count
                                                  : 1;
      out.bookCount = static_cast<uint8_t>(out.bookCount + block.count);
      break;
    }
    case BlockType::Stats:
      for (const JsonVariantConst item : obj["items"].as<JsonArrayConst>()) {
        const char* name = item | "";
        const int stat = indexOf(kStatNames, name);
        if (stat < 0 || block.count >= kMaxStats) {
          LOG_ERR("THM", "%s: block %d: stat %s %s, ignored", file, index, name, stat < 0 ? "unknown" : "past 4");
          continue;
        }
        block.stats[block.count++] = static_cast<Stat>(stat);
      }
      break;
    case BlockType::Menu:
      parseMenu(obj, file, index, block, out);
      break;
    case BlockType::Text:
      if (out.textCount >= kMaxTexts || !copyText(out.texts[out.textCount], kTextBytes, obj["text"] | "")) {
        LOG_ERR("THM", "%s: block %d: text needs 1 to %u bytes, at most %d text blocks; ignored", file, index,
                static_cast<unsigned>(kTextBytes - 1), kMaxTexts);
        return;
      }
      block.first = out.textCount++;
      block.count = 1;
      break;
    case BlockType::Image: {
      const char* name = obj["file"] | "";
      if (out.imageCount >= kMaxImages || !validPackFile(name) ||
          !copyText(out.images[out.imageCount], kTextBytes, name)) {
        LOG_ERR("THM", "%s: block %d: image \"%s\" must be a file in the pack folder, at most %d images; ignored", file,
                index, name, kMaxImages);
        return;
      }
      block.first = out.imageCount++;
      block.count = 1;
      break;
    }
    case BlockType::Header:
    case BlockType::Clock:
    case BlockType::Spacer:
      block.count = 1;
      break;
  }
  if (block.count == 0) {
    LOG_ERR("THM", "%s: block %d: %s has no items, ignored", file, index, typeName);
    return;
  }
  out.blocks[out.blockCount++] = block;
}

// Settings must stay reachable: append it to the last menu, or add a menu for it.
void ensureSettings(const char* file, Layout& out) {
  for (uint8_t i = 0; i < out.entryCount; ++i) {
    if (out.entries[i].action == Action::Settings) return;
  }
  LOG_INF("THM", "%s: no settings entry, appended", file);
  Block* menu = nullptr;
  for (uint8_t i = 0; i < out.blockCount; ++i) {
    if (out.blocks[i].type == BlockType::Menu) menu = &out.blocks[i];
  }
  // The last menu's entries end at entryCount (menus take entries in order).
  if (!menu) {
    menu = &out.blocks[out.blockCount++];
    *menu = Block{};
    menu->type = BlockType::Menu;
    menu->first = out.entryCount;
  }
  out.entries[out.entryCount++] = Entry{};  // Action::Settings
  ++menu->count;
}

void scalePercents(const char* file, Layout& out) {
  int total = 0;
  for (uint8_t i = 0; i < out.blockCount; ++i) {
    if (out.blocks[i].height != kFlex) total += out.blocks[i].height;
  }
  if (total <= 100) return;
  LOG_ERR("THM", "%s: heights add up to %d%%, scaled to 100%%", file, total);
  for (uint8_t i = 0; i < out.blockCount; ++i) {
    uint8_t& height = out.blocks[i].height;
    if (height != 0 && height != kFlex) height = static_cast<uint8_t>(std::max(1, height * 100 / total));
  }
}

}  // namespace

bool parse(const JsonObjectConst root, const char* file, Layout& out) {
  out = Layout{};
  for (const JsonPairConst kv : root) {
    if (std::strcmp(kv.key().c_str(), "blocks") != 0) {
      LOG_ERR("THM", "%s: key %s not supported, ignored", file, kv.key().c_str());
    }
  }
  const JsonArrayConst blocks = root["blocks"].as<JsonArrayConst>();
  if (blocks.isNull()) {
    LOG_ERR("THM", "%s: no \"blocks\" list", file);
    return false;
  }
  int index = 0;
  for (const JsonVariantConst value : blocks) {
    ++index;
    // One block stays free for the menu ensureSettings() may add.
    if (out.blockCount >= kMaxBlocks - 1) {
      LOG_ERR("THM", "%s: more than %d blocks, the rest ignored", file, kMaxBlocks - 1);
      break;
    }
    if (!value.is<JsonObjectConst>()) {
      LOG_ERR("THM", "%s: block %d is not an object, ignored", file, index);
      continue;
    }
    parseBlock(value.as<JsonObjectConst>(), file, index, out);
  }
  if (out.blockCount == 0) {
    LOG_ERR("THM", "%s: no usable blocks", file);
    return false;
  }
  ensureSettings(file, out);
  scalePercents(file, out);
  return true;
}

bool parsePage(const JsonObjectConst root, const char* file, const char* id, Layout& out) {
  out = Layout{};
  for (const JsonPairConst kv : root) {
    const char* key = kv.key().c_str();
    if (std::strcmp(key, "title") && std::strcmp(key, "style") && std::strcmp(key, "columns") &&
        std::strcmp(key, "items")) {
      LOG_ERR("THM", "%s: key %s not supported, ignored", file, key);
    }
  }
  if (!copyText(out.texts[0], kTextBytes, root["title"] | "")) {
    if (!root["title"].isNull()) {
      LOG_ERR("THM", "%s: title needs 1 to %u bytes, using %s", file, static_cast<unsigned>(kTextBytes - 1), id);
    }
    std::snprintf(out.texts[0], kTextBytes, "%s", id);
  }
  out.textCount = 1;
  // parseEntry() keeps the last entry for Home's Settings; a page has Back instead.
  parseMenu(root, file, 1, out.blocks[0], out);
  if (out.blocks[0].count == 0) {
    LOG_ERR("THM", "%s: no usable items", file);
    return false;
  }
  out.blockCount = 1;
  return true;
}

}  // namespace ThemePackHome
