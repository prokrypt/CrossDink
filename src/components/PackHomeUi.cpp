#include "PackHomeUi.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ThemePack.h"
#include "UITheme.h"
#include "UIThemeTokens.h"
#include "activities/reader/BookReadingStats.h"
#include "components/HeaderDate.h"
#include "components/KOSyncBadge.h"
#include "components/icons/listIcons.h"
#include "components/icons/readingStatsIcons.h"
#include "util/LocalClock.h"

namespace fui = freeink::ui;
namespace TPH = ThemePackHome;

namespace {
constexpr fui::ActionId SELECT = 1;
constexpr int16_t COVER_INSET = 6;

UIIcon iconOf(const TPH::Action action) {
  switch (action) {
    case TPH::Action::Continue:
    case TPH::Action::Book:
      return UIIcon::Book;
    case TPH::Action::Library:
      return UIIcon::Library;
    case TPH::Action::Files:
    case TPH::Action::Folder:
    case TPH::Action::Page:
      return UIIcon::Folder;
    case TPH::Action::Opds:
      return UIIcon::Opds;
    case TPH::Action::Transfer:
      return UIIcon::Transfer;
    case TPH::Action::Settings:
      return UIIcon::Settings;
    case TPH::Action::Stats:
      return UIIcon::Chart;
    case TPH::Action::Saved:
      return UIIcon::BookmarkIcon;
    case TPH::Action::Goodies:
      return UIIcon::Flask;
  }
  return UIIcon::File;
}

// The pack's icon first, then the built-in one (listIconFor covers the rest).
fui::BitmapRef entryIcon(const UIIcon icon, const int size) {
  if (const freeink::Icon* packIcon = ThemePack::icon(icon, size)) return fui::bitmapFromIcon(*packIcon);
  const bool big = size >= 32;
  switch (icon) {
    case UIIcon::Settings:
      return fui::bitmapFromIcon(big ? icon_lyra_settings_32 : icon_lyra_settings_24);
    case UIIcon::Transfer:
      return fui::bitmapFromIcon(big ? icon_lyra_transfer_32 : icon_lyra_transfer_24);
    case UIIcon::BookmarkIcon:
      return fui::bitmapFromIcon(big ? icon_bookmark_32 : icon_bookmark_24);
    case UIIcon::Flask:
      return fui::bitmapFromIcon(big ? icon_flask_32 : icon_flask_24);
    case UIIcon::Chart:
      return fui::bitmapFromIcon(icon_reading_stats_32);  // one size only
    default:
      return listIconFor(icon, size);
  }
}

// Book cards and covers: a 2 px outline when selected, not a fill (fewer changed
// pixels, less ghosting), as Cover Grid draws them.
fui::StyleSet coverStyles(const fui::ThemeTokens& theme) {
  fui::StyleSet styles = theme.listRow;
  styles.selected.background = fui::Paint::solid(fui::Color::White);
  styles.selected.border = uiSelectionBorder();
  styles.selected.borderWidth = 2;
  styles.selected.foreground = fui::Paint::solid(fui::Color::Black);
  styles.selected.radius = theme.listRowRadius;
  styles.active = styles.selected;
  return styles;
}

int16_t centeredY(const fui::Rect rect, const int height) {
  return static_cast<int16_t>(rect.y + std::max(0, (rect.height - height) / 2));
}
}  // namespace

PackHomeUi::PackHomeUi(GfxRenderer& renderer, const Layout& source, const int16_t top)
    : UiAppHost(renderer), top(top), layout(source), coverSlots(renderer), renderer(renderer) {}

void PackHomeUi::begin(const std::vector<RecentBook>& recent, const Available has) {
  books = &recent;
  available = has;
  progress.fill(-1.0f);
  reset();
  app.on(SELECT, &PackHomeUi::onAction, this);
  app.setScreen(&PackHomeUi::screenFn, this);
  coverSlots.begin(recent);

  // Covers blocks take the recent books in order; entries with nothing to open are left out.
  stopTotal = 0;
  size_t nextBook = 0;
  for (uint8_t b = 0; b < layout.blockCount; ++b) {
    const auto& block = layout.blocks[b];
    blockStart[b] = static_cast<uint8_t>(stopTotal);
    if (block.type == TPH::BlockType::Covers) {
      for (int k = 0; k < block.count && nextBook < recent.size(); ++k) {
        stops[stopTotal++] = Stop{b, static_cast<uint8_t>(nextBook++)};
      }
    } else if (block.type == TPH::BlockType::Menu) {
      for (uint8_t e = block.first; e < block.first + block.count; ++e) {
        const auto action = layout.entries[e].action;
        if ((action == TPH::Action::Continue && !has.continueReading) ||
            (action == TPH::Action::Saved && !has.bookmarks && !has.clippings) ||
            (action == TPH::Action::Opds && !has.opds) || (action == TPH::Action::Stats && !has.stats)) {
          continue;
        }
        stops[stopTotal++] = Stop{b, e};
      }
    }
    blockStops[b] = static_cast<uint8_t>(stopTotal - blockStart[b]);
  }
  selected = 0;
}

bool PackHomeUi::shows(const TPH::Stat stat) const {
  for (uint8_t b = 0; b < layout.blockCount; ++b) {
    const auto& block = layout.blocks[b];
    if (block.type != TPH::BlockType::Stats) continue;
    if (std::find(block.stats, block.stats + block.count, stat) != block.stats + block.count) return true;
  }
  return false;
}

bool PackHomeUi::showsCard(const int book) const {
  const int stop = stopForBook(book);
  return stop >= 0 && layout.blocks[stops[stop].block].style == TPH::Style::Single;
}

void PackHomeUi::select(const int stop) {
  if (stop >= 0 && stop < stopTotal) selected = stop;
}

int PackHomeUi::stopForBook(const int book) const {
  for (int i = 0; i < stopTotal; ++i) {
    if (layout.blocks[stops[i].block].type == TPH::BlockType::Covers && stops[i].item == book) return i;
  }
  return -1;
}

int PackHomeUi::stopForAction(const TPH::Action action) const {
  for (int i = 0; i < stopTotal; ++i) {
    if (layout.blocks[stops[i].block].type == TPH::BlockType::Menu && layout.entries[stops[i].item].action == action) {
      return i;
    }
  }
  return -1;
}

PackHomeUi::Target PackHomeUi::targetAt(const int stop) const {
  if (stop < 0 || stop >= stopTotal) return {};
  const Stop& s = stops[stop];
  if (layout.blocks[s.block].type == TPH::BlockType::Covers) return Target{s.item, nullptr};
  return Target{-1, &layout.entries[s.item]};
}

void PackHomeUi::onAction(const fui::ActionEvent& event, void* user) {
  auto& self = *static_cast<PackHomeUi*>(user);
  self.pending = event.value;
  self.app.clearTapFlash();
}

int PackHomeUi::tappedStop(const MappedInputManager& input) {
  pending = -1;
  fui::ActionEvent event{};
  return routeTouch(input, event) && pending < stopTotal ? pending : -1;
}

void PackHomeUi::screenFn(UiScreen& screen, void* user) { static_cast<PackHomeUi*>(user)->draw(screen); }

void PackHomeUi::draw(UiScreen& screen) {
  coverSlots.prepare();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int areaTop = top >= 0 ? top : UITheme::getHeaderRect(renderer, 0).y;
  const int side = metrics.contentSidePadding;
  const fui::Rect area{static_cast<int16_t>(safe.x + side), static_cast<int16_t>(areaTop),
                       static_cast<int16_t>(safe.width - 2 * side),
                       static_cast<int16_t>(safe.y + safe.height - areaTop)};
  if (area.empty()) return;
  fui::layoutLinear(
      area, fui::Axis::Column, screen.theme().spaceSm, layout.blockCount,
      [&](const uint8_t b) { return lengthOf(screen, b, area.height); },
      [&](const uint8_t b, fui::Rect rect) {
        // Heights past the area are cut there, never drawn under the button hints.
        rect.height = static_cast<int16_t>(std::min<int>(rect.height, area.bottom() - rect.y));
        if (!rect.empty()) drawBlock(screen, b, rect);
      });
}

fui::LayoutLength PackHomeUi::lengthOf(const UiScreen& screen, const int b, const int16_t areaHeight) const {
  const auto& block = layout.blocks[b];
  if (block.height == TPH::kFlex) return fui::LayoutLength::flexible();
  if (block.height > 0) return fui::LayoutLength::fixed(static_cast<int16_t>(areaHeight * block.height / 100));
  const auto& theme = screen.theme();
  const auto& target = const_cast<UiScreen&>(screen).target();
  switch (block.type) {
    case TPH::BlockType::Header:
      return fui::LayoutLength::fixed(static_cast<int16_t>(UITheme::getInstance().getMetrics().homeTopPadding));
    case TPH::BlockType::Stats:
      // metricCard: label, value, its padding and gap.
      return fui::LayoutLength::fixed(
          static_cast<int16_t>(target.lineHeight(theme.smallText.font) + target.lineHeight(theme.titleText.font) + 16));
    case TPH::BlockType::Clock:
      return fui::LayoutLength::fixed(static_cast<int16_t>(target.lineHeight(theme.titleText.font) +
                                                           target.lineHeight(theme.smallText.font) + theme.spaceSm));
    case TPH::BlockType::Text:
      return fui::LayoutLength::fixed(static_cast<int16_t>(target.lineHeight(theme.bodyText.font) + theme.spaceSm));
    case TPH::BlockType::Image: {
      const freeink::Icon* image = ThemePack::image(block.first);
      return fui::LayoutLength::fixed(static_cast<int16_t>(image ? image->h : 0));
    }
    case TPH::BlockType::Covers:
    case TPH::BlockType::Menu:
    case TPH::BlockType::Spacer:
      break;
  }
  return fui::LayoutLength::flexible();
}

void PackHomeUi::drawBlock(UiScreen& screen, const int b, const fui::Rect rect) {
  const auto& block = layout.blocks[b];
  const auto& theme = screen.theme();
  switch (block.type) {
    case TPH::BlockType::Header: {
      Rect header = UITheme::getHeaderRect(renderer, rect.height);
      header.y = rect.y;
      GUI.drawHeader(renderer, header, nullptr);
      return;
    }
    case TPH::BlockType::Covers:
      if (blockStops[b] == 0) {
        // No recent book yet: the first covers block says so, as Cover Grid does.
        if (!books->empty()) return;
        for (int k = 0; k < b; ++k) {
          if (layout.blocks[k].type == TPH::BlockType::Covers) return;
        }
        auto title = theme.bodyText;
        title.bold = true;
        title.align = fui::TextAlign::Center;
        auto message = theme.smallText;
        message.align = fui::TextAlign::Center;
        const int16_t titleH = screen.target().lineHeight(title.font);
        const int16_t messageH = screen.target().lineHeight(message.font);
        const int16_t y = centeredY(rect, titleH + theme.spaceSm + messageH);
        screen.target().text(fui::Rect{rect.x, y, rect.width, titleH}, tr(STR_NO_OPEN_BOOK), title);
        screen.target().text(fui::Rect{rect.x, static_cast<int16_t>(y + titleH + theme.spaceSm), rect.width, messageH},
                             tr(STR_START_READING), message);
      } else if (block.style == TPH::Style::Single) {
        drawCard(screen, blockStart[b], rect);
      } else {
        drawCoverGrid(screen, b, rect);
      }
      return;
    case TPH::BlockType::Stats:
      drawStats(screen, block, rect);
      return;
    case TPH::BlockType::Menu:
      if (blockStops[b] == 0) return;
      if (block.style == TPH::Style::Grid) {
        drawTiles(screen, b, rect);
      } else {
        drawList(screen, b, rect);
      }
      return;
    case TPH::BlockType::Clock:
      drawClock(screen, rect);
      return;
    case TPH::BlockType::Text: {
      auto style = theme.bodyText;
      style.align = fui::TextAlign::Center;
      const int16_t lineH = screen.target().lineHeight(style.font);
      FilenameUiFontScope fonts(target, renderer);  // per-glyph fallbacks for the pack's own text
      screen.target().text(fui::Rect{rect.x, centeredY(rect, lineH), rect.width, lineH}, layout.texts[block.first],
                           style);
      return;
    }
    case TPH::BlockType::Image:
      if (const freeink::Icon* image = ThemePack::image(block.first)) {
        const bool fits = image->w <= rect.width && image->h <= rect.height;
        screen.target().bitmap(rect, fui::bitmapFromIcon(*image),
                               fits ? fui::BitmapMode::Center : fui::BitmapMode::Contain,
                               fui::Paint::solid(fui::Color::Black));
      }
      return;
    case TPH::BlockType::Spacer:
      return;
  }
}

void PackHomeUi::drawCard(UiScreen& screen, const int stop, const fui::Rect rect) {
  const auto& theme = screen.theme();
  const int book = stops[stop].item;
  const RecentBook& recent = (*books)[book];
  const int percent = progress[book] >= 0 ? static_cast<int>(progress[book] + 0.5f) : -1;
  if (percent >= 0) snprintf(cardText, sizeof(cardText), "%d%%", percent);
  card = fui::BookCardProps{};
  card.title = recent.title.c_str();
  card.author = recent.author.empty() ? nullptr : recent.author.c_str();
  card.progressLabel = percent >= 0 ? cardText : nullptr;
  card.centerTextOnCover = true;
  card.progress = std::max(0, percent);
  card.progressMax = percent >= 0 ? 100 : 0;
  card.action = SELECT;
  card.value = static_cast<int16_t>(stop);
  card.state = stop == selected ? fui::StateSelected : fui::StateNormal;
  card.styles = coverStyles(theme);
  card.titleText = theme.bodyText;
  card.titleText.maxLines = renderer.getScreenWidth() > renderer.getScreenHeight() ? 1 : 2;
  card.authorText = theme.smallText;
  card.progressText = theme.smallText;
  card.progressHeight = 6;
  card.padding = fui::Insets{6, 6, 6, 6};
  card.gap = static_cast<int16_t>(theme.spaceLg + theme.spaceSm);
  card.coverSize.height = static_cast<int16_t>(
      std::max(1, std::min<int>(rect.height - 2 * COVER_INSET, (rect.width / 3 - 2 * COVER_INSET) * 3 / 2)));
  card.coverSize.width = static_cast<int16_t>(std::max(1, card.coverSize.height * 2 / 3));
  coverSlots.note(book, card.coverSize.width, card.coverSize.height);
  card.coverPainterUserData = this;
  card.coverPainter = [](fui::DrawTarget& target, const fui::Rect cover, const fui::BookCardProps& props, void* user) {
    auto& self = *static_cast<PackHomeUi*>(user);
    return self.coverSlots.paintFramed(target, cover, self.stops[props.value].item);
  };
  {
    FilenameUiFontScope fonts(target, renderer);
    fui::bookCard(screen.frame(), rect, card);
  }
  // The most recent book's KOSync badge sits above its %, as on Cover Grid (bookCard()'s geometry).
  if (book == 0 && card.progressLabel) {
    const fui::Rect content = rect.inset(card.padding);
    const int coverW = std::min<int>(card.coverSize.width, content.width);
    const int coverH = std::min<int>(card.coverSize.height, content.height);
    const int coverBottom = content.y + (content.height - coverH) / 2 + coverH;
    const int progressRowH = std::max<int>(screen.target().lineHeight(card.progressText.font), card.progressHeight);
    KOSyncBadge::drawAt(renderer, content.x + coverW + card.gap, coverBottom - progressRowH - KOSyncBadge::SIZE,
                        KOSyncBadge::SIZE);
  }
}

void PackHomeUi::drawCoverGrid(UiScreen& screen, const int b, const fui::Rect rect) {
  const auto& block = layout.blocks[b];
  // Sized for the block's full count, so slot (and thumb) sizes do not change with how many books there are.
  const int columns = std::max<int>(1, block.columns);
  const int rows = (block.count + columns - 1) / columns;
  const int16_t gap = static_cast<int16_t>(std::max(4, rect.width / 100));
  const int cellW = (rect.width - (columns - 1) * gap) / columns;
  const int rowH = (rect.height - (rows - 1) * gap) / std::max(1, rows);
  const int coverH = std::max(1, std::min(rowH - 2 * COVER_INSET, (cellW - 2 * COVER_INSET) * 3 / 2));
  grid = fui::CoverGridProps{};
  grid.count = blockStops[b];
  grid.columns = static_cast<uint8_t>(columns);
  grid.columnLayout = fui::CoverGridColumnLayout::SpaceBetween;
  grid.gap = gap;
  grid.rowGap = gap;
  grid.cellInset = fui::Insets{COVER_INSET, COVER_INSET, COVER_INSET, COVER_INSET};
  grid.coverSize = fui::Size{static_cast<int16_t>(std::max(1, coverH * 2 / 3)), static_cast<int16_t>(coverH)};
  grid.rowHeight = static_cast<int16_t>(coverH + 2 * COVER_INSET);
  grid.labelHeight = 0;
  grid.labelGap = 0;
  grid.scrollIndicator = false;
  grid.action = SELECT;
  grid.inputMask = fui::InputTouch;
  grid.selectedIndex = static_cast<int16_t>(selectedIn(b) ? selected - blockStart[b] : -1);
  grid.selectionIndicator = fui::CoverGridSelectionIndicator::CoverFrame;
  grid.selectedCoverFrameGap = 6;
  grid.selectedCoverFrameWidth = 8;
  grid.cellStyles = coverStyles(screen.theme());
  for (int k = 0; k < grid.count; ++k) {
    coverSlots.note(stops[blockStart[b] + k].item, grid.coverSize.width, grid.coverSize.height);
  }
  drawingBlock = b;
  grid.itemProviderUserData = this;
  grid.itemProvider = [](const uint16_t index, void* user) {
    const auto& self = *static_cast<PackHomeUi*>(user);
    return fui::coverGridItem(nullptr, self.blockStart[self.drawingBlock] + index);
  };
  grid.coverPainterUserData = this;
  grid.coverPainter = [](fui::DrawTarget& target, const fui::Rect cover, const fui::CoverGridItem& item, uint16_t,
                         void* user) {
    auto& self = *static_cast<PackHomeUi*>(user);
    return self.coverSlots.paintFramed(target, cover, self.stops[item.actionValue].item);
  };
  const int used = rows * grid.rowHeight + (rows - 1) * gap;
  fui::coverGrid(
      screen.frame(),
      fui::Rect{rect.x, centeredY(rect, used), rect.width, static_cast<int16_t>(std::min<int>(used, rect.height))},
      grid);
}

void PackHomeUi::drawStats(UiScreen& screen, const TPH::Block& block, const fui::Rect rect) {
  const auto& theme = screen.theme();
  metric = fui::MetricCardProps{};
  metric.labelText = theme.smallText;
  metric.valueText = theme.titleText;
  metric.valueText.bold = true;
  const int16_t gap = theme.spaceSm;
  const int16_t cardW = static_cast<int16_t>((rect.width - (block.count - 1) * gap) / block.count);
  for (int i = 0; i < block.count; ++i) {
    char* value = text[i];
    metric.unit = nullptr;
    switch (block.stats[i]) {
      case TPH::Stat::Streak:
        snprintf(value, sizeof(text[i]), "%u", static_cast<unsigned>(stats.streak));
        metric.unit = stats.streak == 1 ? tr(STR_STATS_DAY) : tr(STR_STATS_DAYS);
        metric.label = tr(STR_STATS_READING_STREAK_LBL);
        break;
      case TPH::Stat::Today:
        if (stats.hasToday) {
          BookReadingStats::formatDuration(stats.todaySeconds, value, sizeof(text[i]));
        } else {
          snprintf(value, sizeof(text[i]), "-");
        }
        metric.label = tr(STR_STATS_TODAY_LBL);
        break;
      case TPH::Stat::TotalTime:
        BookReadingStats::formatDuration(stats.totalSeconds, value, sizeof(text[i]));
        metric.label = tr(STR_STATS_ALL_TIME);
        break;
      case TPH::Stat::BooksRead:
        snprintf(value, sizeof(text[i]), "%lu", static_cast<unsigned long>(stats.booksRead));
        metric.label = tr(STR_STATS_COMPLETED_LBL);
        break;
      case TPH::Stat::BookProgress:
        if (stats.bookProgress >= 0) {
          snprintf(value, sizeof(text[i]), "%d%%", static_cast<int>(stats.bookProgress + 0.5f));
        } else {
          snprintf(value, sizeof(text[i]), "-");
        }
        metric.label = tr(STR_STATS_PROGRESS_LBL);
        break;
      case TPH::Stat::BookTime:
        BookReadingStats::formatDuration(stats.bookSeconds, value, sizeof(text[i]));
        metric.label = tr(STR_STATS_TIME_LBL);
        break;
    }
    metric.value = value;
    fui::metricCard(screen.frame(),
                    fui::Rect{static_cast<int16_t>(rect.x + i * (cardW + gap)), rect.y, cardW, rect.height}, metric);
  }
}

void PackHomeUi::drawList(UiScreen& screen, const int b, const fui::Rect rect) {
  drawingBlock = b;
  list = fui::ListProps{};
  list.count = blockStops[b];
  list.action = SELECT;
  list.iconSize = 24;
  list.selectedIndex = static_cast<int16_t>(selectedIn(b) ? selected - blockStart[b] : -1);
  list.rowProviderCtx = this;
  list.rowProvider = [](void* ctx, const uint16_t index, fui::ListItem& item) {
    const auto& self = *static_cast<PackHomeUi*>(ctx);
    const int stop = self.blockStart[self.drawingBlock] + index;
    const auto& entry = self.layout.entries[self.stops[stop].item];
    item.label = self.entryLabel(entry);
    item.icon = entryIcon(iconOf(entry.action), 24);
    item.actionValue = static_cast<int16_t>(stop);
  };
  const fui::ListProps themed = screen.resolveListProps(list);
  // Pages by selection: the page holding the focus is shown.
  const int visible = std::max<int>(
      1, fui::listVisibleRows(rect, themed.rowHeight > 0 ? themed.rowHeight : 36, std::max<int16_t>(0, themed.rowGap)));
  fui::ListProps paged = themed;
  paged.topIndex = static_cast<uint16_t>(std::max(0, static_cast<int>(themed.selectedIndex)) / visible * visible);
  fui::list(screen.frame(), rect, paged);
}

void PackHomeUi::drawTiles(UiScreen& screen, const int b, const fui::Rect rect) {
  const auto& theme = screen.theme();
  const int count = blockStops[b];
  for (int k = 0; k < count; ++k) {
    const int stop = blockStart[b] + k;
    const auto& entry = layout.entries[stops[stop].item];
    auto& item = tileItems[k];
    item = fui::TileGridItem{};
    item.label = entryLabel(entry);
    item.icon = entryIcon(iconOf(entry.action), 32);
    item.value = static_cast<int16_t>(stop);
    item.state = stop == selected ? fui::StateSelected : fui::StateNormal;
  }
  const auto& block = layout.blocks[b];
  const int rows = (count + block.columns - 1) / block.columns;
  tiles = fui::TileGridProps{};
  tiles.items = tileItems.data();
  tiles.count = static_cast<uint16_t>(count);
  tiles.action = SELECT;
  tiles.columns = block.columns;
  tiles.gap = theme.spaceSm;
  // Rows share the block, up to two touch targets tall each.
  tiles.tileHeight = static_cast<int16_t>(
      std::max(1, std::min<int>((rect.height - (rows - 1) * tiles.gap) / rows, theme.minTouchSize * 2)));
  tiles.text = theme.smallText;
  tiles.iconSize = 32;
  tiles.radius = theme.controlRadius;
  fui::tileGrid(screen.frame(), rect, tiles);
}

void PackHomeUi::drawClock(UiScreen& screen, const fui::Rect rect) {
  char time[16];
  if (!halClock.isAvailable() ||
      !halClock.formatTime(time, sizeof(time), LocalClock::currentOffsetQ(), SETTINGS.clockFormat == 1)) {
    return;
  }
  char date[40];
  const bool hasDate = formatHeaderDateText(date, sizeof(date));
  const auto& theme = screen.theme();
  auto timeStyle = theme.titleText;
  timeStyle.bold = true;
  timeStyle.align = fui::TextAlign::Center;
  auto dateStyle = theme.smallText;
  dateStyle.align = fui::TextAlign::Center;
  const int16_t timeH = screen.target().lineHeight(timeStyle.font);
  const int16_t dateH = hasDate ? screen.target().lineHeight(dateStyle.font) : 0;
  const int16_t y = centeredY(rect, timeH + dateH);
  screen.target().text(fui::Rect{rect.x, y, rect.width, timeH}, time, timeStyle);
  if (hasDate)
    screen.target().text(fui::Rect{rect.x, static_cast<int16_t>(y + timeH), rect.width, dateH}, date, dateStyle);
}

const char* PackHomeUi::entryLabel(const TPH::Entry& entry) const {
  switch (entry.action) {
    case TPH::Action::Continue:
      return tr(STR_CONTINUE_READING);
    case TPH::Action::Library:
      return tr(STR_LIBRARY);
    case TPH::Action::Files:
      return tr(STR_BROWSE_FILES);
    case TPH::Action::Opds:
      return tr(STR_OPDS_BROWSER);
    case TPH::Action::Transfer:
      return tr(STR_FILE_TRANSFER);
    case TPH::Action::Settings:
      return tr(STR_SETTINGS_TITLE);
    case TPH::Action::Stats:
      return tr(STR_READING_STATS);
    case TPH::Action::Saved:
      return available.bookmarks && available.clippings ? tr(STR_BOOKMARKS_AND_CLIPPINGS)
             : available.clippings                      ? tr(STR_CLIPPINGS)
                                                        : tr(STR_BOOKMARKS);
    case TPH::Action::Goodies:
      return tr(STR_GOODIES);
    case TPH::Action::Page:
      return entry.path;  // the page id
    case TPH::Action::Folder:
    case TPH::Action::Book: {
      // The last path part: "/Comics" shows as Comics.
      const char* slash = std::strrchr(entry.path, '/');
      return slash && slash[1] != '\0' ? slash + 1 : entry.path;
    }
  }
  return "";
}
