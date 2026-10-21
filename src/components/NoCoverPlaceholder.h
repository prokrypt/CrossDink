#pragma once

#include "RecentBooksStore.h"

class GfxRenderer;

// The placeholder for a book with no cover: the bold title, a short rule and
// the author. It draws over what is there, with no fill and no frame, so each
// caller keeps its own frame shape.
namespace NoCoverPlaceholder {
void draw(const GfxRenderer& renderer, int x, int y, int width, int height, const RecentBook& book);
}  // namespace NoCoverPlaceholder
