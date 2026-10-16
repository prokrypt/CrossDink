#include "NoCoverPlaceholder.h"

#include <GfxRenderer.h>

#include <algorithm>
#include <string>
#include <vector>

#include "fontIds.h"

namespace NoCoverPlaceholder {

void draw(const GfxRenderer& renderer, const int x, const int y, const int width, const int height,
          const RecentBook& book) {
  constexpr int kSpineInset = 12;
  constexpr int kSpineGap = 3;
  constexpr int kTextGap = 14;
  constexpr int kPadRight = 10;
  constexpr int kPadTop = 16;
  constexpr int kPadBottom = 14;
  constexpr int kRuleGap = 8;
  constexpr int kRuleWidth = 28;
  constexpr int kLargeMinWidth = 140;  // narrower tiles (Lyra 3 Covers, grids) step the fonts down

  const int spineX = x + kSpineInset;
  renderer.drawLine(spineX, y, spineX, y + height - 1, true);
  renderer.drawLine(spineX + kSpineGap, y, spineX + kSpineGap, y + height - 1, true);

  const int textX = spineX + kSpineGap + kTextGap;
  const int textW = x + width - kPadRight - textX;
  if (textW < 24) return;

  const bool large = width >= kLargeMinWidth;
  const int titleFont = large ? UI_12_FONT_ID : UI_10_FONT_ID;
  const int authorFont = large ? UI_10_FONT_ID : SMALL_FONT_ID;
  const int titleH = renderer.getLineHeight(titleFont);
  const int authorH = renderer.getLineHeight(authorFont);

  const auto authorLines = book.author.empty() ? std::vector<std::string>{}
                                               : renderer.wrappedText(authorFont, book.author.c_str(), textW, 2);
  const int authorY = y + height - kPadBottom - authorH * static_cast<int>(authorLines.size());

  const int titleY = y + kPadTop;
  const int maxTitleLines = std::min(4, (authorY - kRuleGap * 2 - titleY) / titleH);
  if (maxTitleLines > 0) {
    const char* title = book.title.empty() ? book.path.c_str() : book.title.c_str();
    int lineY = titleY;
    for (const auto& line : renderer.wrappedText(titleFont, title, textW, maxTitleLines, EpdFontFamily::BOLD)) {
      renderer.drawText(titleFont, textX, lineY, line.c_str(), true, EpdFontFamily::BOLD);
      lineY += titleH;
    }
    renderer.fillRect(textX, lineY + kRuleGap, kRuleWidth, 2, true);
  }

  int lineY = authorY;
  for (const auto& line : authorLines) {
    renderer.drawText(authorFont, textX, lineY, line.c_str());
    lineY += authorH;
  }
}

}  // namespace NoCoverPlaceholder
