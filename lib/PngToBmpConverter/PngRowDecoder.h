#pragma once

#include <HalStorage.h>
#include <InflateStream.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>

// Streaming PNG scanline decoder, the firmware's only PNG decoder: covers and
// thumbnails (PngToBmpConverter), inline EPUB images (PngToFramebufferConverter)
// and sleep-screen overlays all read rows through it, on the same miniz
// InflateStream that unpacks EPUB zip entries.
//
// Reads non-interlaced PNGs of every color type and bit depth from an open
// file or a memory buffer. open() parses IHDR and keeps PLTE/tRNS; begin()
// allocates the row buffers and the inflate stream; nextRow() returns one
// defiltered scanline in the PNG's own sample layout.
class PngRowDecoder {
 public:
  enum ColorType : uint8_t { Gray = 0, Rgb = 2, Palette = 3, GrayAlpha = 4, Rgba = 6 };
  // Defiltered rows larger than this are refused (an RGBA 8-bit row of 4096 px).
  static constexpr uint32_t kMaxRowBytes = 16384;

  PngRowDecoder() = default;
  PngRowDecoder(const PngRowDecoder&) = delete;
  PngRowDecoder& operator=(const PngRowDecoder&) = delete;

  // `file` must stay open, positioned anywhere, until decoding ends.
  bool openFile(FsFile& file);
  // `data` must outlive the decoder.
  bool openMemory(const uint8_t* data, size_t size);

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  uint8_t bitDepth() const { return bitDepth_; }
  ColorType colorType() const { return colorType_; }
  // Bytes in one defiltered scanline (no filter byte).
  uint32_t rowBytes() const { return rowBytes_; }

  // Palette in PNGdec's layout: RGB triplets at [0, 768) and per-entry alpha
  // at [768, 1024), 255 where tRNS gives none. Null for non-palette images.
  const uint8_t* palette() const { return colorType_ == Palette ? palette_ : nullptr; }
  int paletteEntries() const { return paletteEntries_; }
  // True when pixels can be transparent: an alpha channel or a tRNS chunk.
  bool hasAlpha() const { return colorType_ == GrayAlpha || colorType_ == Rgba || hasTrns_; }
  // tRNS color key for Gray/Rgb images, -1 if none: 0x00RRGGBB for Rgb, the
  // gray value in the low byte for Gray (low byte of each 16-bit sample).
  int32_t transparentColor() const { return transparentColor_; }

  // Allocate the scanline buffers and inflate stream. Call once after open().
  bool begin();
  // The next defiltered scanline (rowBytes() bytes), or nullptr on a corrupt
  // or truncated stream. Valid until the next call.
  const uint8_t* nextRow();

  // Sample x of a Gray or Palette row with 1/2/4/8-bit samples (MSB first).
  static uint8_t sample(const uint8_t* row, uint32_t x, uint8_t bitDepth) {
    if (bitDepth == 8) return row[x];
    const uint32_t bitOffset = x * bitDepth;
    const int shift = 8 - bitDepth - static_cast<int>(bitOffset & 7);
    return (row[bitOffset >> 3] >> shift) & ((1U << bitDepth) - 1);
  }
  // A Gray sample scaled to 0-255.
  static uint8_t sampleToByte(const uint8_t value, const uint8_t bitDepth) {
    if (bitDepth >= 8) return value;
    return static_cast<uint8_t>((value * 255U) / ((1U << bitDepth) - 1));
  }

 private:
  bool readBytes(uint8_t* out, size_t len);
  bool skipBytes(size_t len);
  bool readBE32(uint32_t& value);
  bool parseHeaderAndChunks();
  static size_t fillIdat(void* ctx, const uint8_t** data);

  // Source: exactly one of file_ / mem_ is set.
  FsFile* file_ = nullptr;
  const uint8_t* mem_ = nullptr;
  size_t memSize_ = 0;
  size_t memPos_ = 0;

  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint8_t bitDepth_ = 0;
  ColorType colorType_ = Gray;
  uint8_t bytesPerPixel_ = 1;  // filter stride; 1 for sub-byte depths
  uint32_t rowBytes_ = 0;

  // One allocation per open(): the 1024-byte palette plus a 2 KB file read
  // buffer (empty for memory sources). Heap, so the decoder stays small enough
  // for any task stack.
  HeapByteBuffer meta_;
  uint8_t* palette_ = nullptr;
  uint8_t* readBuf_ = nullptr;
  static constexpr size_t kReadBufBytes = 2048;
  int paletteEntries_ = 0;
  bool hasTrns_ = false;
  int32_t transparentColor_ = -1;

  uint32_t idatRemaining_ = 0;
  bool idatFinished_ = false;
  InflateStream inflate_;
  HeapByteBuffer rows_;  // current + previous scanline
  uint8_t* current_ = nullptr;
  uint8_t* previous_ = nullptr;
  bool started_ = false;
};
