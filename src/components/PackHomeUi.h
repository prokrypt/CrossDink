#pragma once

#include <array>
#include <vector>

#include "HomeCoverCache.h"
#include "RecentBooksStore.h"
#include "ThemePackHome.h"
#include "UiAppHost.h"
#include "components/lists/list.h"
#include "components/lists/tile-grid.h"
#include "components/media/book-card.h"
#include "components/media/cover-grid.h"
#include "components/media/metric-card.h"

// Home laid out by the active theme pack's home.json (ThemePackHome.h): its blocks
// stacked over the Home area and drawn with FreeInkUI components. User pages
// (PackPageActivity) draw their one menu block with it too. Recent books and
// menu entries are the focus stops, in block order and left to right inside a
// block. HomeActivity owns the focus and opens what is picked; drawing reads only
// RAM (covers come from the PSRAM cover cache once drawn).
class PackHomeUi final : public UiAppHost<kUiLargeInteractions, kUiLargeHandlers> {
 public:
  using UiScreen = UiAppHost<kUiLargeInteractions, kUiLargeHandlers>::Screen;
  using Layout = ThemePackHome::Layout;
  static_assert(ThemePackHome::kMaxBooks <= HomeCoverCache::MAX_COVERS, "one cover slot per shown book");

  // Stats block values, gathered by Home so drawing reads no card.
  struct Stats {
    uint16_t streak = 0;
    bool hasToday = false;
    uint32_t todaySeconds = 0;
    uint32_t totalSeconds = 0;
    uint32_t booksRead = 0;
    float bookProgress = -1.0f;  // the most recent book's
    uint32_t bookSeconds = 0;
  };
  // What a focus stop opens: a recent book, or a menu entry.
  struct Target {
    int book = -1;
    const ThemePackHome::Entry* entry = nullptr;
  };
  // Which built-in entries have something to open; the others are left out.
  struct Available {
    bool continueReading = false;
    bool bookmarks = false;
    bool clippings = false;
    bool opds = false;
    bool stats = false;
  };

  // top: where the blocks start; -1 for Home's (under the status header). User pages
  // pass the bottom of their back-button header.
  PackHomeUi(GfxRenderer& renderer, const Layout& layout, int16_t top = -1);
  void begin(const std::vector<RecentBook>& books, Available available);
  bool shows(ThemePackHome::Stat stat) const;
  bool showsCard(int book) const;  // a single-style covers block shows it, with its %
  int stopCount() const { return stopTotal; }
  void select(int stop);  // render task: Home keeps the stop in its selectorIndex
  int stopForBook(int book) const;
  int stopForAction(ThemePackHome::Action action) const;
  Target targetAt(int stop) const;  // not target(): UiAppHost::target is the draw target
  // The stop a tap picked, or -1.
  int tappedStop(const MappedInputManager& input);
  HomeCoverSlots& covers() { return coverSlots; }
  int bookCount() const { return layout.bookCount; }
  const Available& has() const { return available; }
  Stats stats;  // written by Home before each render
  // Book cards' %, -1 when unknown; begin() clears it and Home fills it for showsCard() books.
  std::array<float, ThemePackHome::kMaxBooks> progress{};

 private:
  struct Stop {
    uint8_t block = 0;
    uint8_t item = 0;  // covers: book index; menu: entry index
  };
  static void screenFn(UiScreen& screen, void* user);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void draw(UiScreen& screen);
  freeink::ui::LayoutLength lengthOf(const UiScreen& screen, int block, int16_t areaHeight) const;
  void drawBlock(UiScreen& screen, int block, freeink::ui::Rect rect);
  void drawCard(UiScreen& screen, int stop, freeink::ui::Rect rect);
  void drawCoverGrid(UiScreen& screen, int block, freeink::ui::Rect rect);
  void drawStats(UiScreen& screen, const ThemePackHome::Block& block, freeink::ui::Rect rect);
  void drawList(UiScreen& screen, int block, freeink::ui::Rect rect);
  void drawTiles(UiScreen& screen, int block, freeink::ui::Rect rect);
  void drawClock(UiScreen& screen, freeink::ui::Rect rect);
  const char* entryLabel(const ThemePackHome::Entry& entry) const;
  bool selectedIn(int block) const {
    return selected >= blockStart[block] && selected < blockStart[block] + blockStops[block];
  }

  int16_t top;
  Layout layout;  // a copy: Home outlives a pack reload under the Settings drawer
  HomeCoverSlots coverSlots;
  GfxRenderer& renderer;
  const std::vector<RecentBook>* books = nullptr;
  Available available;
  std::array<Stop, ThemePackHome::kMaxBooks + ThemePackHome::kMaxEntries> stops{};
  std::array<uint8_t, ThemePackHome::kMaxBlocks> blockStart{};
  std::array<uint8_t, ThemePackHome::kMaxBlocks> blockStops{};
  int stopTotal = 0;
  int selected = 0;
  int pending = -1;
  int drawingBlock = 0;  // the block whose cover grid or list is being drawn (provider callbacks)
  // Component props and items stay off the render task's stack.
  freeink::ui::BookCardProps card;
  freeink::ui::CoverGridProps grid;
  freeink::ui::MetricCardProps metric;
  freeink::ui::ListProps list;
  freeink::ui::TileGridProps tiles;
  std::array<freeink::ui::TileGridItem, ThemePackHome::kMaxEntries> tileItems;
  char text[ThemePackHome::kMaxStats][24]{};
  char cardText[8]{};
};
