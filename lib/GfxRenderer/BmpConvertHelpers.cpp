#include "BmpConvertHelpers.h"

#include <Print.h>

namespace bmpconvert {

namespace {
constexpr uint32_t FP_ONE = 1UL << 16;
}  // namespace

static void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

static void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

static void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

static uint32_t fpPerOutputPixel(const uint64_t srcSpan_fp, const int outPixels) {
  if (outPixels <= 0) return FP_ONE;
  const uint64_t value = srcSpan_fp / static_cast<uint64_t>(outPixels);
  if (value == 0) return 1;
  if (value > UINT32_MAX) return UINT32_MAX;
  return static_cast<uint32_t>(value);
}

OutputGeometry calculateOutputGeometry(const int srcWidth, const int srcHeight, const int targetWidth,
                                       const int targetHeight, const bool crop) {
  OutputGeometry geometry{srcWidth, srcHeight, FP_ONE, FP_ONE, 0, 0, false};
  if (targetWidth <= 0 || targetHeight <= 0 || srcWidth <= 0 || srcHeight <= 0) {
    return geometry;
  }

  if (crop) {
    geometry.outWidth = targetWidth;
    geometry.outHeight = targetHeight;

    const uint64_t srcWidth_fp = static_cast<uint64_t>(srcWidth) << 16;
    const uint64_t srcHeight_fp = static_cast<uint64_t>(srcHeight) << 16;
    uint64_t cropWidth_fp = srcWidth_fp;
    uint64_t cropHeight_fp = srcHeight_fp;
    const int64_t sourceVsTarget =
        static_cast<int64_t>(srcWidth) * targetHeight - static_cast<int64_t>(targetWidth) * srcHeight;

    if (sourceVsTarget > 0) {
      cropWidth_fp = (static_cast<uint64_t>(targetWidth) * static_cast<uint64_t>(srcHeight) << 16) / targetHeight;
      if (cropWidth_fp > srcWidth_fp) cropWidth_fp = srcWidth_fp;
      geometry.srcXOffset_fp = static_cast<uint32_t>((srcWidth_fp - cropWidth_fp) / 2);
    } else if (sourceVsTarget < 0) {
      cropHeight_fp = (static_cast<uint64_t>(targetHeight) * static_cast<uint64_t>(srcWidth) << 16) / targetWidth;
      if (cropHeight_fp > srcHeight_fp) cropHeight_fp = srcHeight_fp;
      geometry.srcYOffset_fp = static_cast<uint32_t>((srcHeight_fp - cropHeight_fp) / 2);
    }

    geometry.scaleX_fp = fpPerOutputPixel(cropWidth_fp, targetWidth);
    geometry.scaleY_fp = fpPerOutputPixel(cropHeight_fp, targetHeight);
    geometry.needsScaling = srcWidth != targetWidth || srcHeight != targetHeight || geometry.srcXOffset_fp != 0 ||
                            geometry.srcYOffset_fp != 0;
    return geometry;
  }

  if (srcWidth != targetWidth || srcHeight != targetHeight) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    const float scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;

    geometry.outWidth = static_cast<int>(srcWidth * scale);
    geometry.outHeight = static_cast<int>(srcHeight * scale);
    if (geometry.outWidth < 1) geometry.outWidth = 1;
    if (geometry.outHeight < 1) geometry.outHeight = 1;

    geometry.scaleX_fp = fpPerOutputPixel(static_cast<uint64_t>(srcWidth) << 16, geometry.outWidth);
    geometry.scaleY_fp = fpPerOutputPixel(static_cast<uint64_t>(srcHeight) << 16, geometry.outHeight);
    geometry.needsScaling = true;
  }

  return geometry;
}

bool shouldContainAdaptive(const int srcWidth, const int srcHeight, const int targetWidth, const int targetHeight) {
  if (srcWidth <= 0 || srcHeight <= 0 || targetWidth <= 0 || targetHeight <= 0) {
    return false;
  }

  constexpr int64_t kAspectTolerancePercent = 18;
  const int64_t sourceScaledToTargetHeight = static_cast<int64_t>(srcWidth) * targetHeight;
  const int64_t targetScaledToSourceHeight = static_cast<int64_t>(targetWidth) * srcHeight;
  const int64_t diff = sourceScaledToTargetHeight > targetScaledToSourceHeight
                           ? sourceScaledToTargetHeight - targetScaledToSourceHeight
                           : targetScaledToSourceHeight - sourceScaledToTargetHeight;
  return diff * 100 > targetScaledToSourceHeight * kAspectTolerancePercent;
}

}  // namespace bmpconvert
