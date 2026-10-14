#include "PngToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"
#include "BmpConvertHelpers.h"
#include "Memory.h"
#include "PngRowDecoder.h"

using bmpconvert::calculateOutputGeometry;
using bmpconvert::OutputGeometry;
using bmpconvert::shouldContainAdaptive;
using bmpconvert::writeBmpHeader1bit;
using bmpconvert::writeBmpHeader2bit;
using bmpconvert::writeBmpHeader8bit;

// ============================================================================
// IMAGE PROCESSING OPTIONS - Same as JpegToBmpConverter for consistency
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;
constexpr bool USE_ATKINSON = true;
constexpr bool USE_FLOYD_STEINBERG = false;
constexpr bool USE_PRESCALE = true;
// ============================================================================

namespace {
void yieldDuringDecode(uint8_t& rowsSinceYield) {
  if (++rowsSinceYield < 8) return;
  rowsSinceYield = 0;
  vTaskDelay(1);
}

}  // namespace

// Batch-convert an entire scanline to grayscale.
// Branches once on colorType/bitDepth, then runs a tight loop for the whole row.
static void convertScanlineToGray(const PngRowDecoder& png, const uint8_t* src, uint8_t* grayRow) {
  const uint32_t w = png.width();
  const uint8_t bitDepth = png.bitDepth();

  switch (png.colorType()) {
    case PngRowDecoder::Gray:
      if (bitDepth == 8) {
        memcpy(grayRow, src, w);
      } else if (bitDepth == 16) {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 2];
      } else {
        const int ppb = 8 / bitDepth;
        const uint8_t mask = (1 << bitDepth) - 1;
        for (uint32_t x = 0; x < w; x++) {
          int shift = (ppb - 1 - (x % ppb)) * bitDepth;
          grayRow[x] = (src[x / ppb] >> shift & mask) * 255 / mask;
        }
      }
      break;

    case PngRowDecoder::Rgb:
      if (bitDepth == 8) {
        // Fast path: most common EPUB cover format
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* p = src + x * 3;
          grayRow[x] = (p[0] * 25 + p[1] * 50 + p[2] * 25) / 100;
        }
      } else {
        for (uint32_t x = 0; x < w; x++) {
          grayRow[x] = (src[x * 6] * 25 + src[x * 6 + 2] * 50 + src[x * 6 + 4] * 25) / 100;
        }
      }
      break;

    case PngRowDecoder::Palette: {
      const int ppb = 8 / bitDepth;
      const uint8_t mask = (1 << bitDepth) - 1;
      const uint8_t* pal = png.palette();
      const int palSize = png.paletteEntries();
      for (uint32_t x = 0; x < w; x++) {
        int shift = (ppb - 1 - (x % ppb)) * bitDepth;
        uint8_t idx = (src[x / ppb] >> shift) & mask;
        if (idx >= palSize) idx = 0;
        grayRow[x] = (pal[idx * 3] * 25 + pal[idx * 3 + 1] * 50 + pal[idx * 3 + 2] * 25) / 100;
      }
      break;
    }

    case PngRowDecoder::GrayAlpha:
      if (bitDepth == 8) {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 2];
      } else {
        for (uint32_t x = 0; x < w; x++) grayRow[x] = src[x * 4];
      }
      break;

    case PngRowDecoder::Rgba:
      if (bitDepth == 8) {
        for (uint32_t x = 0; x < w; x++) {
          const uint8_t* p = src + x * 4;
          grayRow[x] = (p[0] * 25 + p[1] * 50 + p[2] * 25) / 100;
        }
      } else {
        for (uint32_t x = 0; x < w; x++) {
          grayRow[x] = (src[x * 8] * 25 + src[x * 8 + 2] * 50 + src[x * 8 + 4] * 25) / 100;
        }
      }
      break;

    default:
      memset(grayRow, 128, w);
      break;
  }
}

bool PngToBmpConverter::pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                   bool oneBit, bool crop, bool adaptiveContain, bool imageLevels) {
  PngRowDecoder png;
  if (!png.openFile(pngFile)) return false;
  const uint32_t width = png.width();
  const uint32_t height = png.height();

  // Safety limits
  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (width > MAX_IMAGE_WIDTH || height > MAX_IMAGE_HEIGHT) {
    LOG_ERR("PNG", "Image too large (%" PRIu32 "x%" PRIu32 ")", width, height);
    return false;
  }

  // Scanline rows, inflate state and 32KB window for the whole conversion.
  if (!png.begin()) return false;

  // Calculate output dimensions. Crop mode behaves like CSS object-fit: cover:
  // scale to fill the requested box, then sample a centered source crop before dithering.
  const bool containInsteadOfCrop =
      crop && adaptiveContain &&
      shouldContainAdaptive(static_cast<int>(width), static_cast<int>(height), targetWidth, targetHeight);
  const bool cropOutput = crop && !containInsteadOfCrop;
  const OutputGeometry geometry =
      calculateOutputGeometry(static_cast<int>(width), static_cast<int>(height), targetWidth, targetHeight, cropOutput);
  const int outWidth = geometry.outWidth;
  const int outHeight = geometry.outHeight;
  const bool needsScaling = geometry.needsScaling;

  // Write BMP header
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  // The encoded and grayscale rows are also used together for the whole
  // conversion, so keep them in one automatically owned scratch allocation.
  const size_t rowScratchBytes = static_cast<size_t>(bytesPerRow) + static_cast<size_t>(width);
  auto rowScratch = makeUniqueNoThrow<uint8_t[]>(rowScratchBytes);
  if (!rowScratch) {
    LOG_ERR("PNG", "OOM: row scratch buffer (%u bytes)", static_cast<unsigned>(rowScratchBytes));
    return false;
  }
  uint8_t* rowBuffer = rowScratch.get();
  uint8_t* grayRow = rowBuffer + bytesPerRow;

  // Create ditherers (same as JpegToBmpConverter)
  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  if (oneBit) {
    atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!atkinson1BitDitherer || !atkinson1BitDitherer->isValid()) {
      LOG_ERR("PNG", "OOM: Atkinson1BitDitherer or row buffers");
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth, imageLevels);
      if (!atkinsonDitherer || !atkinsonDitherer->isValid()) {
        LOG_ERR("PNG", "OOM: AtkinsonDitherer or row buffers");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth, imageLevels);
      if (!fsDitherer || !fsDitherer->isValid()) {
        LOG_ERR("PNG", "OOM: FloydSteinbergDitherer or row buffers");
        return false;
      }
    }
  }

  // Scaling accumulators
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;
  int currentOutY = 0;
  uint32_t nextOutY_srcStart = 0;

  if (needsScaling) {
    rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!rowAccum || !rowCount) {
      LOG_ERR("PNG", "OOM: scaling accumulators");
      return false;
    }
    nextOutY_srcStart = geometry.srcYOffset_fp + geometry.scaleY_fp;
  }

  bool success = true;
  uint8_t rowsSinceYield = 0;

  // Process each scanline
  for (uint32_t y = 0; y < height; y++) {
    // Decode one scanline
    const uint8_t* scanline = png.nextRow();
    if (!scanline) {
      LOG_ERR("PNG", "Failed to decode scanline %" PRIu32, y);
      success = false;
      break;
    }

    // Batch-convert entire scanline to grayscale (one branch, tight loop)
    convertScanlineToGray(png, scanline, grayRow);

    if (!needsScaling) {
      // Direct output (no scaling)
      memset(rowBuffer, 0, bytesPerRow);

      if (USE_8BIT_OUTPUT && !oneBit) {
        for (int x = 0; x < outWidth; x++) {
          rowBuffer[x] = adjustPixel(grayRow[x]);
        }
      } else if (oneBit) {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t bit =
              atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(grayRow[x], x) : quantize1bit(grayRow[x], x, y);
          const int byteIndex = x / 8;
          const int bitOffset = 7 - (x % 8);
          rowBuffer[byteIndex] |= (bit << bitOffset);
        }
        if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
      } else {
        for (int x = 0; x < outWidth; x++) {
          const uint8_t gray = adjustPixel(grayRow[x]);
          uint8_t twoBit;
          if (atkinsonDitherer) {
            twoBit = atkinsonDitherer->processPixel(gray, x);
          } else if (fsDitherer) {
            twoBit = fsDitherer->processPixel(gray, x);
          } else {
            twoBit = quantize(gray, x, y);
          }
          const int byteIndex = (x * 2) / 8;
          const int bitOffset = 6 - ((x * 2) % 8);
          rowBuffer[byteIndex] |= (twoBit << bitOffset);
        }
        if (atkinsonDitherer)
          atkinsonDitherer->nextRow();
        else if (fsDitherer)
          fsDitherer->nextRow();
      }
      bmpOut.write(rowBuffer, bytesPerRow);
      yieldDuringDecode(rowsSinceYield);
    } else {
      const uint64_t srcY_fp = static_cast<uint64_t>(y + 1) << 16;
      if (srcY_fp <= geometry.srcYOffset_fp) {
        continue;
      }

      // Area-averaging scaling (same as JpegToBmpConverter)
      for (int outX = 0; outX < outWidth; outX++) {
        const uint64_t srcXStart_fp =
            static_cast<uint64_t>(geometry.srcXOffset_fp) + static_cast<uint64_t>(outX) * geometry.scaleX_fp;
        const uint64_t srcXEnd_fp =
            static_cast<uint64_t>(geometry.srcXOffset_fp) + static_cast<uint64_t>(outX + 1) * geometry.scaleX_fp;
        const int srcXStart = std::min(static_cast<int>(width) - 1, static_cast<int>(srcXStart_fp >> 16));
        const int srcXEnd = std::min(static_cast<int>(width), static_cast<int>(srcXEnd_fp >> 16));

        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < static_cast<int>(width); srcX++) {
          sum += grayRow[srcX];
          count++;
        }

        if (count == 0 && srcXStart < static_cast<int>(width)) {
          sum = grayRow[srcXStart];
          count = 1;
        }

        rowAccum[outX] += sum;
        rowCount[outX] += count;
      }

      // Check if we've crossed into the next output row(s)
      // Output all rows whose boundaries we've crossed (handles both up and downscaling)
      // For upscaling, one source row may produce multiple output rows
      while (srcY_fp >= nextOutY_srcStart && currentOutY < outHeight) {
        memset(rowBuffer, 0, bytesPerRow);

        if (USE_8BIT_OUTPUT && !oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            rowBuffer[x] = adjustPixel(gray);
          }
        } else if (oneBit) {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = (rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0;
            const uint8_t bit =
                atkinson1BitDitherer ? atkinson1BitDitherer->processPixel(gray, x) : quantize1bit(gray, x, currentOutY);
            const int byteIndex = x / 8;
            const int bitOffset = 7 - (x % 8);
            rowBuffer[byteIndex] |= (bit << bitOffset);
          }
          if (atkinson1BitDitherer) atkinson1BitDitherer->nextRow();
        } else {
          for (int x = 0; x < outWidth; x++) {
            const uint8_t gray = adjustPixel((rowCount[x] > 0) ? (rowAccum[x] / rowCount[x]) : 0);
            uint8_t twoBit;
            if (atkinsonDitherer) {
              twoBit = atkinsonDitherer->processPixel(gray, x);
            } else if (fsDitherer) {
              twoBit = fsDitherer->processPixel(gray, x);
            } else {
              twoBit = quantize(gray, x, currentOutY);
            }
            const int byteIndex = (x * 2) / 8;
            const int bitOffset = 6 - ((x * 2) % 8);
            rowBuffer[byteIndex] |= (twoBit << bitOffset);
          }
          if (atkinsonDitherer)
            atkinsonDitherer->nextRow();
          else if (fsDitherer)
            fsDitherer->nextRow();
        }

        bmpOut.write(rowBuffer, bytesPerRow);
        currentOutY++;
        yieldDuringDecode(rowsSinceYield);

        nextOutY_srcStart = static_cast<uint32_t>(static_cast<uint64_t>(geometry.srcYOffset_fp) +
                                                  static_cast<uint64_t>(currentOutY + 1) * geometry.scaleY_fp);

        // For upscaling: don't reset accumulators if next output row uses same source data
        // Only reset when we'll move to a new source row
        if (srcY_fp >= nextOutY_srcStart) {
          // More output rows to emit from same source - keep accumulator data
          continue;
        }
        // Moving to next source row - reset accumulators
        memset(rowAccum.get(), 0, outWidth * sizeof(uint32_t));
        memset(rowCount.get(), 0, outWidth * sizeof(uint32_t));
      }
    }
  }

  if (success) {
  }
  return success;
}

bool PngToBmpConverter::pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop, bool imageLevels) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetWidth, targetHeight, false, crop, false, imageLevels);
}

bool PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth,
                                                       int targetMaxHeight, bool adaptiveContain) {
  return pngFileToBmpStreamInternal(pngFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true, adaptiveContain);
}
