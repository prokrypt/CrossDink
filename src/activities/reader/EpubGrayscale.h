#pragma once

#include <cstddef>
#include <cstdint>

class GfxRenderer;
class Page;

namespace EpubGrayscale {
constexpr int GRAYSCALE_STRIP_ROWS = 80;

// Preserves the live BW buffer and existing controller synchronization. False
// leaves the caller responsible for its existing BW-snapshot fallback.
// grayscaleShown, when given, reports whether the gray planes reached the panel
// (true can still mean a queued turn cancelled the pass). shouldCancel gets the
// name of the point it is checked at, for the cancel log.
bool runTiledGrayscalePass(GfxRenderer& renderer, const Page& page, int fontId, int marginLeft, int marginTop,
                           bool foregroundBlack, bool needsTextGrayscale, bool needsImageGrayscale, uint8_t* scratch,
                           size_t scratchSize, bool asyncRefreshPending,
                           bool (*shouldCancel)(void*, const char* checkpoint) = nullptr, void* cancelContext = nullptr,
                           bool* grayscaleShown = nullptr);
}  // namespace EpubGrayscale
