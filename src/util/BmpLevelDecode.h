#pragma once

#include <atomic>

class Bitmap;
class GfxRenderer;
class HalFile;

// Decodes a parsed BMP once into DirectPixelWriter::levelPlanes (set by the caller), placed as
// GfxRenderer::drawBitmap(bitmap, x, y, maxWidth, maxHeight, cropX, cropY) places it. The pixel
// data comes from one PSRAM read when it fits; rows split across the cores when the caller is not
// on the worker core. Stops early once *cancel is set (the caller checks it). False on a read error.
bool decodeBmpLevelPlanes(GfxRenderer& renderer, Bitmap& bitmap, HalFile& file, int x, int y, int maxWidth,
                          int maxHeight, float cropX, float cropY, const std::atomic<bool>* cancel = nullptr);
