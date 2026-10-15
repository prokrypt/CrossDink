#include "Lyra3CoversTheme.h"

#include <GfxRenderer.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "RecentBooksStore.h"
#include "components/KOSyncBadge.h"
#include "components/NoCoverPlaceholder.h"
#include "components/TouchRegistry.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

// Internal constants
namespace {
constexpr int hPaddingInSelection = 8;
constexpr int cornerRadius = 6;
// Render task only: Home sets these right before drawRecentBookCover().
float tileProgress[Lyra3CoversMetrics::values.homeRecentBooksCount] = {-1.0f, -1.0f, -1.0f};
}  // namespace

void Lyra3CoversTheme::setTileProgress(const int slot, const float progressPercent) {
  if (slot >= 0 && slot < Lyra3CoversMetrics::values.homeRecentBooksCount) tileProgress[slot] = progressPercent;
}

void Lyra3CoversTheme::drawRecentBookCover(GfxRenderer& renderer, Rect rect, const std::vector<RecentBook>& recentBooks,
                                           int selectorIndex, bool& coverRendered, bool& coverBufferStored,
                                           bool& bufferRestored, const std::function<bool()>& storeCoverBuffer,
                                           const BookReadingStats* /*stats*/, float /*progressPercent*/,
                                           const GlobalReadingStats* /*globalStats*/,
                                           const char* /*currentChapterTitle*/) const {
  const int tileWidth = (rect.width - 2 * themed(Lyra3CoversMetrics::values).contentSidePadding) / 3;
  const int tileY = rect.y;
  const bool hasContinueReading = !recentBooks.empty();

  // Draw book card regardless, fill with message based on `hasContinueReading`
  // Draw cover image as background if available (inside the box)
  // Only load from SD on first render, then use stored buffer
  if (hasContinueReading) {
    if (!coverRendered) {
      for (int i = 0;
           i < std::min(static_cast<int>(recentBooks.size()), themed(Lyra3CoversMetrics::values).homeRecentBooksCount);
           i++) {
        std::string coverPath = recentBooks[i].coverBmpPath;
        bool hasCover = true;
        int tileX = themed(Lyra3CoversMetrics::values).contentSidePadding + tileWidth * i;
        if (coverPath.empty()) {
          hasCover = false;
        } else {
          const std::string coverBmpPath =
              UITheme::getCoverThumbPath(coverPath, themed(Lyra3CoversMetrics::values).homeCoverHeight);

          // First time: load cover from SD and render
          HalFile file;
          if (Storage.openFileForRead("HOME", coverBmpPath, file)) {
            Bitmap bitmap(file);
            if (bitmap.parseHeaders() == BmpReaderError::Ok) {
              float coverHeight = static_cast<float>(bitmap.getHeight());
              float coverWidth = static_cast<float>(bitmap.getWidth());
              float ratio = coverWidth / coverHeight;
              const float tileRatio = static_cast<float>(tileWidth - 2 * hPaddingInSelection) /
                                      static_cast<float>(themed(Lyra3CoversMetrics::values).homeCoverHeight);
              float cropX = 1.0f - (tileRatio / ratio);

              renderer.drawBitmap(bitmap, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                  tileWidth - 2 * hPaddingInSelection,
                                  themed(Lyra3CoversMetrics::values).homeCoverHeight, cropX);
            } else {
              hasCover = false;
            }
            file.close();
          }
        }
        // Draw either way
        renderer.drawRect(tileX + hPaddingInSelection, tileY + hPaddingInSelection, tileWidth - 2 * hPaddingInSelection,
                          themed(Lyra3CoversMetrics::values).homeCoverHeight, true);

        if (!hasCover) {
          NoCoverPlaceholder::draw(renderer, tileX + hPaddingInSelection, tileY + hPaddingInSelection,
                                   tileWidth - 2 * hPaddingInSelection, themed(Lyra3CoversMetrics::values).homeCoverHeight,
                                   recentBooks[i]);
        }
      }

      coverBufferStored = storeCoverBuffer();
      coverRendered = coverBufferStored;  // Only consider it rendered if we successfully stored the buffer
    }

    for (int i = 0;
         i < std::min(static_cast<int>(recentBooks.size()), themed(Lyra3CoversMetrics::values).homeRecentBooksCount);
         i++) {
      bool bookSelected = (selectorIndex == i);

      int tileX = themed(Lyra3CoversMetrics::values).contentSidePadding + tileWidth * i;
      const int targetHeight = themed(Lyra3CoversMetrics::values).homeCoverHeight + hPaddingInSelection +
                               renderer.getLineHeight(SMALL_FONT_ID) * 3;
      TouchRegistry::getInstance().add(Rect{tileX, tileY, tileWidth, targetHeight}, i, TouchRegistry::Cover);

      const int maxLineWidth = tileWidth - 2 * hPaddingInSelection;

      auto titleLines = renderer.wrappedText(SMALL_FONT_ID, recentBooks[i].title.c_str(), maxLineWidth, 3);

      const int titleLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
      const int dynamicBlockHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
      // Add a little padding below the text inside the selection box just like the top padding (5 + hPaddingSelection)
      // The book's % and then its KOSync badge sit on one row below the title, inside the selection box.
      char progressLabel[8] = "";
      if (tileProgress[i] >= 0.0f) snprintf(progressLabel, sizeof(progressLabel), "%.0f%%", tileProgress[i]);
      tileProgress[i] = -1.0f;
      const int statusRowHeight = std::max(titleLineHeight, static_cast<int>(KOSyncBadge::SIZE));
      const bool hasStatusRow = progressLabel[0] || KOSyncBadge::hasHomeState(i);
      const int dynamicTitleBoxHeight =
          dynamicBlockHeight + (hasStatusRow ? statusRowHeight : 0) + hPaddingInSelection + 5;

      if (bookSelected) {
        // Selection box: 2 px 50% gray outline around cover and title, not a dither fill.
        renderer.drawRoundedRect(
            tileX, tileY, tileWidth,
            themed(Lyra3CoversMetrics::values).homeCoverHeight + hPaddingInSelection + dynamicTitleBoxHeight, 2, cornerRadius,
            Color::DarkGray);
      }

      int currentY = tileY + themed(Lyra3CoversMetrics::values).homeCoverHeight + hPaddingInSelection + 5;
      for (const auto& line : titleLines) {
        renderer.drawText(SMALL_FONT_ID, tileX + hPaddingInSelection, currentY, line.c_str(), true);
        currentY += titleLineHeight;
      }
      int badgeX = tileX + hPaddingInSelection;
      if (progressLabel[0]) {
        renderer.drawText(SMALL_FONT_ID, badgeX, currentY + (statusRowHeight - titleLineHeight) / 2, progressLabel,
                          true);
        badgeX += renderer.getTextWidth(SMALL_FONT_ID, progressLabel) + KOSyncBadge::GAP;
      }
      KOSyncBadge::drawAt(renderer, badgeX, currentY, statusRowHeight, true, i);
    }
  } else {
    drawEmptyRecents(renderer, rect);
  }
}
