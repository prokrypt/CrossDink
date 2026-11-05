#include "PngRowDecoder.h"

#include <Logging.h>

#include <cstring>
#include <utility>

namespace {
constexpr uint8_t kSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

enum Filter : uint8_t { FilterNone = 0, FilterSub = 1, FilterUp = 2, FilterAverage = 3, FilterPaeth = 4 };

uint8_t paeth(const uint8_t a, const uint8_t b, const uint8_t c) {
  const int p = static_cast<int>(a) + b - c;
  const int pa = p > a ? p - a : a - p;
  const int pb = p > b ? p - b : b - p;
  const int pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

bool validDepth(const uint8_t colorType, const uint8_t depth) {
  switch (colorType) {
    case PngRowDecoder::Gray:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PngRowDecoder::Palette:
      return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case PngRowDecoder::Rgb:
    case PngRowDecoder::GrayAlpha:
    case PngRowDecoder::Rgba:
      return depth == 8 || depth == 16;
    default:
      return false;
  }
}

uint8_t channels(const uint8_t colorType) {
  switch (colorType) {
    case PngRowDecoder::Rgb:
      return 3;
    case PngRowDecoder::GrayAlpha:
      return 2;
    case PngRowDecoder::Rgba:
      return 4;
    default:
      return 1;
  }
}
}  // namespace

bool PngRowDecoder::readBytes(uint8_t* out, const size_t len) {
  if (file_) return file_->read(out, len) == static_cast<int>(len);
  if (len > memSize_ - memPos_) return false;
  memcpy(out, mem_ + memPos_, len);
  memPos_ += len;
  return true;
}

bool PngRowDecoder::skipBytes(const size_t len) {
  if (file_) return file_->seekCur(static_cast<int>(len));
  if (len > memSize_ - memPos_) return false;
  memPos_ += len;
  return true;
}

bool PngRowDecoder::readBE32(uint32_t& value) {
  uint8_t b[4];
  if (!readBytes(b, 4)) return false;
  value = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) |
          (static_cast<uint32_t>(b[2]) << 8) | b[3];
  return true;
}

bool PngRowDecoder::openFile(FsFile& file) {
  file_ = &file;
  if (!file.seek(0)) {
    LOG_ERR("PNG", "Cannot rewind PNG file");
    return false;
  }
  return parseHeaderAndChunks();
}

bool PngRowDecoder::openMemory(const uint8_t* data, const size_t size) {
  mem_ = data;
  memSize_ = data ? size : 0;
  memPos_ = 0;
  return parseHeaderAndChunks();
}

bool PngRowDecoder::parseHeaderAndChunks() {
  // Palette (1 KB) and, for files, the IDAT read buffer (2 KB) share one
  // allocation that lives as long as the decoder.
  meta_ = makeHeapByteBufferNoThrow(1024 + (file_ ? kReadBufBytes : 0));
  if (!meta_) {
    LOG_ERR("PNG", "OOM: PNG decoder buffers");
    return false;
  }
  palette_ = meta_.get();
  readBuf_ = file_ ? meta_.get() + 1024 : nullptr;
  memset(palette_, 0, 768);
  memset(palette_ + 768, 0xFF, 256);

  uint8_t sig[8];
  if (!readBytes(sig, sizeof(sig)) || memcmp(sig, kSignature, sizeof(sig)) != 0) {
    LOG_ERR("PNG", "Invalid PNG signature");
    return false;
  }
  uint32_t len = 0;
  uint8_t type[4];
  uint8_t ihdr[13];
  if (!readBE32(len) || !readBytes(type, 4) || memcmp(type, "IHDR", 4) != 0 || len != sizeof(ihdr) ||
      !readBytes(ihdr, sizeof(ihdr)) || !skipBytes(4)) {
    LOG_ERR("PNG", "Missing or malformed IHDR");
    return false;
  }
  width_ = (static_cast<uint32_t>(ihdr[0]) << 24) | (static_cast<uint32_t>(ihdr[1]) << 16) |
           (static_cast<uint32_t>(ihdr[2]) << 8) | ihdr[3];
  height_ = (static_cast<uint32_t>(ihdr[4]) << 24) | (static_cast<uint32_t>(ihdr[5]) << 16) |
            (static_cast<uint32_t>(ihdr[6]) << 8) | ihdr[7];
  bitDepth_ = ihdr[8];
  colorType_ = static_cast<ColorType>(ihdr[9]);
  if (ihdr[10] != 0 || ihdr[11] != 0) {
    LOG_ERR("PNG", "Unsupported compression/filter method");
    return false;
  }
  if (ihdr[12] != 0) {
    LOG_ERR("PNG", "Interlaced PNGs not supported");
    return false;
  }
  if (!validDepth(ihdr[9], bitDepth_)) {
    LOG_ERR("PNG", "Unsupported color type %u / bit depth %u", unsigned(ihdr[9]), unsigned(bitDepth_));
    return false;
  }
  if (width_ == 0 || height_ == 0) {
    LOG_ERR("PNG", "Empty PNG");
    return false;
  }
  const uint32_t bitsPerPixel = channels(colorType_) * bitDepth_;
  bytesPerPixel_ = static_cast<uint8_t>(bitsPerPixel >= 8 ? bitsPerPixel / 8 : 1);
  const uint64_t rowBytes = (static_cast<uint64_t>(width_) * bitsPerPixel + 7) / 8;
  if (rowBytes > kMaxRowBytes) {
    LOG_ERR("PNG", "Row too large: %u bytes", unsigned(rowBytes));
    return false;
  }
  rowBytes_ = static_cast<uint32_t>(rowBytes);

  // Collect PLTE and tRNS up to the first IDAT.
  while (true) {
    if (!readBE32(len) || !readBytes(type, 4)) break;
    if (memcmp(type, "IDAT", 4) == 0) {
      idatRemaining_ = len;
      idatFinished_ = false;
      return true;
    }
    if (memcmp(type, "IEND", 4) == 0) break;
    if (memcmp(type, "PLTE", 4) == 0) {
      const uint32_t entries = len / 3 > 256 ? 256 : len / 3;
      if (!readBytes(palette_, entries * 3) || !skipBytes(len - entries * 3 + 4)) break;
      paletteEntries_ = static_cast<int>(entries);
    } else if (memcmp(type, "tRNS", 4) == 0) {
      uint8_t trns[256];
      const uint32_t keep = len > sizeof(trns) ? sizeof(trns) : len;
      if (!readBytes(trns, keep) || !skipBytes(len - keep + 4)) break;
      hasTrns_ = true;
      if (colorType_ == Palette) {
        memcpy(palette_ + 768, trns, keep);
      } else if (colorType_ == Gray && keep >= 2) {
        transparentColor_ = trns[1];
      } else if (colorType_ == Rgb && keep >= 6) {
        transparentColor_ = (static_cast<int32_t>(trns[1]) << 16) | (static_cast<int32_t>(trns[3]) << 8) | trns[5];
      }
    } else if (!skipBytes(static_cast<size_t>(len) + 4)) {
      break;
    }
  }
  LOG_ERR("PNG", "No IDAT chunk found");
  return false;
}

size_t PngRowDecoder::fillIdat(void* ctx, const uint8_t** data) {
  auto& self = *static_cast<PngRowDecoder*>(ctx);
  if (self.idatFinished_) return 0;
  // Step over each finished IDAT's CRC to the next IDAT (they are contiguous).
  while (self.idatRemaining_ == 0) {
    uint32_t len = 0;
    uint8_t type[4];
    if (!self.skipBytes(4) || !self.readBE32(len) || !self.readBytes(type, 4) || memcmp(type, "IDAT", 4) != 0) {
      self.idatFinished_ = true;
      return 0;
    }
    self.idatRemaining_ = len;
  }
  if (self.mem_) {
    // Memory sources hand tinfl the chunk bytes in place.
    const size_t n =
        self.idatRemaining_ < self.memSize_ - self.memPos_ ? self.idatRemaining_ : self.memSize_ - self.memPos_;
    if (n == 0) {
      self.idatFinished_ = true;
      return 0;
    }
    *data = self.mem_ + self.memPos_;
    self.memPos_ += n;
    self.idatRemaining_ -= static_cast<uint32_t>(n);
    return n;
  }
  const size_t want = self.idatRemaining_ < kReadBufBytes ? self.idatRemaining_ : kReadBufBytes;
  const int got = self.file_->read(self.readBuf_, want);
  if (got <= 0) {
    self.idatFinished_ = true;
    return 0;
  }
  self.idatRemaining_ -= static_cast<uint32_t>(got);
  *data = self.readBuf_;
  return static_cast<size_t>(got);
}

bool PngRowDecoder::begin() {
  if (!palette_ || rowBytes_ == 0) return false;
  // Current + previous scanline for the whole decode; at most 2 x kMaxRowBytes.
  rows_ = makeHeapByteBufferNoThrow(static_cast<size_t>(rowBytes_) * 2);
  if (!rows_) {
    LOG_ERR("PNG", "OOM: scanline buffers (%u bytes each)", unsigned(rowBytes_));
    return false;
  }
  current_ = rows_.get();
  previous_ = current_ + rowBytes_;
  memset(previous_, 0, rowBytes_);  // the row above the first one is all zeros
  if (!inflate_.init(true)) {
    LOG_ERR("PNG", "Failed to init inflate stream");
    return false;
  }
  inflate_.setFill(fillIdat, this);
  inflate_.setZlibWrapped();
  started_ = false;
  return true;
}

const uint8_t* PngRowDecoder::nextRow() {
  if (!current_) return nullptr;
  if (started_) std::swap(current_, previous_);  // keep the last returned row as "previous"
  started_ = true;

  uint8_t filter = 0;
  if (!inflate_.read(&filter, 1) || !inflate_.read(current_, rowBytes_)) return nullptr;

  uint8_t* cur = current_;
  const uint8_t* prev = previous_;
  const uint32_t bpp = bytesPerPixel_;
  switch (filter) {
    case FilterNone:
      break;
    case FilterSub:
      for (uint32_t i = bpp; i < rowBytes_; i++) cur[i] += cur[i - bpp];
      break;
    case FilterUp:
      for (uint32_t i = 0; i < rowBytes_; i++) cur[i] += prev[i];
      break;
    case FilterAverage:
      for (uint32_t i = 0; i < rowBytes_; i++) {
        const uint8_t a = i >= bpp ? cur[i - bpp] : 0;
        cur[i] += static_cast<uint8_t>((a + prev[i]) / 2);
      }
      break;
    case FilterPaeth:
      for (uint32_t i = 0; i < rowBytes_; i++) {
        const uint8_t a = i >= bpp ? cur[i - bpp] : 0;
        const uint8_t c = i >= bpp ? prev[i - bpp] : 0;
        cur[i] += paeth(a, prev[i], c);
      }
      break;
    default:
      LOG_ERR("PNG", "Unknown filter type: %u", unsigned(filter));
      return nullptr;
  }
  return cur;
}
