#pragma once

#include <cstdint>

class Print;

// Shared by JpegToBmpConverter and PngToBmpConverter: BMP stream headers and
// the output geometry both converters use to fit or crop an image into a box.
namespace bmpconvert {

// Top-down BMP headers (negative height) for 1-bit, 2-bit (4 grays) and 8-bit
// (256 grays) output; rows follow in the converters.
void writeBmpHeader1bit(Print& out, int width, int height);
void writeBmpHeader2bit(Print& out, int width, int height);
void writeBmpHeader8bit(Print& out, int width, int height);

struct OutputGeometry {
  int outWidth;
  int outHeight;
  uint32_t scaleX_fp;
  uint32_t scaleY_fp;
  uint32_t srcXOffset_fp;
  uint32_t srcYOffset_fp;
  bool needsScaling;
};

// Scale (and, with crop, centre-crop like CSS object-fit: cover) a
// srcWidth x srcHeight image into targetWidth x targetHeight. Steps and offsets
// are 16.16 fixed point in source pixels.
OutputGeometry calculateOutputGeometry(int srcWidth, int srcHeight, int targetWidth, int targetHeight, bool crop);

// True when the source and target aspect ratios differ by more than 18%, so an
// adaptive cover should contain instead of crop.
bool shouldContainAdaptive(int srcWidth, int srcHeight, int targetWidth, int targetHeight);

}  // namespace bmpconvert
