#include "JpegToBmpConverter.h"

#include <Arena.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"
#include "BmpConvertHelpers.h"

using bmpconvert::calculateOutputGeometry;
using bmpconvert::OutputGeometry;
using bmpconvert::shouldContainAdaptive;
using bmpconvert::writeBmpHeader1bit;
using bmpconvert::writeBmpHeader2bit;
using bmpconvert::writeBmpHeader8bit;

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static FsFile* s_jpegFile = nullptr;

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<FsFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  uint32_t srcXOffset_fp;
  uint32_t srcYOffset_fp;
  bool smoothUpscale;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  uint8_t* mcuBuf;

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  uint32_t* rowAccum;
  uint32_t* rowCount;
  int smoothNextOutY;
  int smoothPrevY;
  uint8_t* smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  uint8_t* bmpRow;

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  bool error;
};

// Write a fully-assembled output row (grayscale bytes, length outWidth) to BMP
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      ctx->bmpRow[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(srcRow[x], x)
                                                    : quantize1bit(srcRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel(srcRow[x]);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint64_t srcX_fp = static_cast<uint64_t>(ctx->srcXOffset_fp) + static_cast<uint64_t>(outX) * ctx->scaleX_fp;
    const int x0 = std::min(ctx->srcWidth - 1, static_cast<int>(srcX_fp >> 16));
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = static_cast<uint32_t>(srcX_fp & (FP_ONE - 1));
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
  ctx->currentOutY++;
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
        ctx->currentOutY++;
      }
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint64_t srcY_fp =
        static_cast<uint64_t>(ctx->srcYOffset_fp) + static_cast<uint64_t>(ctx->smoothNextOutY) * ctx->scaleY_fp;
    const int y0 = std::min(ctx->srcHeight - 1, static_cast<int>(srcY_fp >> 16));
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, static_cast<uint32_t>(srcY_fp & (FP_ONE - 1)), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
    ctx->currentOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  memset(ctx->bmpRow, 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      ctx->bmpRow[x] = adjustPixel(gray);
    }
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = (ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0;
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(gray, x)
                                                    : quantize1bit(gray, x, ctx->currentOutY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t gray = adjustPixel((ctx->rowCount[x] > 0) ? (ctx->rowAccum[x] / ctx->rowCount[x]) : 0);
      uint8_t twoBit;
      if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(gray, x);
      } else if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(gray, x);
      } else {
        twoBit = quantize(gray, x, ctx->currentOutY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
    else if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow, ctx->bytesPerRow);
  ctx->currentOutY++;
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Copy block pixels into MCU row buffer
  for (int r = 0; r < blockH && r < MAX_MCU_HEIGHT; r++) {
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf + r * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process each complete source row in this MCU row.
  // Clamp to MAX_MCU_HEIGHT so srcRow never indexes past the populated mcuBuf rows.
  const int safeEndRow = blockY + std::min(blockH, MAX_MCU_HEIGHT);

  for (int y = blockY; y < safeEndRow && y < ctx->srcHeight; y++) {
    const uint8_t* srcRow = ctx->mcuBuf + (y - blockY) * ctx->srcWidth;

    if (ctx->smoothUpscale) {
      processSmoothSourceRow(ctx, srcRow, y);
    } else if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
    } else {
      const uint64_t srcY_fp = static_cast<uint64_t>(y + 1) << 16;
      if (srcY_fp <= ctx->srcYOffset_fp) continue;

      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const uint64_t srcXStart_fp =
            static_cast<uint64_t>(ctx->srcXOffset_fp) + static_cast<uint64_t>(outX) * ctx->scaleX_fp;
        const uint64_t srcXEnd_fp =
            static_cast<uint64_t>(ctx->srcXOffset_fp) + static_cast<uint64_t>(outX + 1) * ctx->scaleX_fp;
        const int srcXStart = std::min(ctx->srcWidth - 1, static_cast<int>(srcXStart_fp >> 16));
        const int srcXEnd = std::min(ctx->srcWidth, static_cast<int>(srcXEnd_fp >> 16));
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(static_cast<uint64_t>(ctx->srcYOffset_fp) +
                                                       static_cast<uint64_t>(ctx->currentOutY + 1) * ctx->scaleY_fp);
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum, 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount, 0, ctx->outWidth * sizeof(uint32_t));
      }
    }
  }

  return ctx->error ? 0 : 1;
}

// Scans JPEG markers for SOF2 (progressive DCT) — JPEGDEC only handles baseline/sequential.
static bool isProgressiveJpeg(FsFile& file) {
  file.seek(0);
  uint8_t buf[2];
  if (file.read(buf, 2) != 2 || buf[0] != 0xFF || buf[1] != 0xD8) {
    file.seek(0);
    return false;
  }
  while (file.available() >= 2) {
    uint8_t b;
    if (file.read(&b, 1) != 1 || b != 0xFF) break;
    // skip fill bytes (JPEG allows 0xFF padding before a marker byte)
    do {
      if (file.read(&b, 1) != 1) {
        file.seek(0);
        return false;
      }
    } while (b == 0xFF);
    const uint8_t marker = b;
    if (marker == 0xC2) {
      LOG_DBG("JPG", "Detected progressive JPEG (SOF2)");
      file.seek(0);
      return true;
    }
    if (marker == 0xC0 || marker == 0xC1 || marker == 0xC3) {
      file.seek(0);
      return false;
    }
    if (marker == 0xD9) break;
    if (file.read(buf, 2) != 2) break;
    const int segLen = (static_cast<int>(buf[0]) << 8) | buf[1];
    if (segLen < 2 || !file.seek(file.position() + segLen - 2)) break;
  }
  file.seek(0);
  return false;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                                     bool oneBit, bool crop, bool adaptiveContain, bool imageLevels) {
  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%" PRIu32 " free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  // Progressive JPEGs (SOF2) must use JPEG_SCALE_EIGHTH — the only mode safe with the MCU_SKIP patch.
  // The 1/8-scale output is then passed through the custom scaler to reach the target dimensions.
  const bool progressive = isProgressiveJpeg(jpegFile);

  s_jpegFile = &jpegFile;

  const auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    return false;
  }

  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  const int effectiveSrcW = progressive ? (srcWidth + 7) / 8 : srcWidth;
  const int effectiveSrcH = progressive ? (srcHeight + 7) / 8 : srcHeight;
  const int decodeFlags = progressive ? JPEG_SCALE_EIGHTH : 0;

  // Calculate output dimensions. Crop mode behaves like CSS object-fit: cover:
  // scale to fill the requested box, then sample a centered source crop before dithering.
  const bool containInsteadOfCrop =
      crop && adaptiveContain && shouldContainAdaptive(effectiveSrcW, effectiveSrcH, targetWidth, targetHeight);
  const bool cropOutput = crop && !containInsteadOfCrop;
  const OutputGeometry geometry =
      calculateOutputGeometry(effectiveSrcW, effectiveSrcH, targetWidth, targetHeight, cropOutput);
  const int outWidth = geometry.outWidth;
  const int outHeight = geometry.outHeight;
  const bool needsScaling = geometry.needsScaling;
  const bool smoothUpscale = progressive && needsScaling && geometry.scaleX_fp <= FP_ONE &&
                             geometry.scaleY_fp <= FP_ONE &&
                             (geometry.scaleX_fp < FP_ONE || geometry.scaleY_fp < FP_ONE);

  // Write BMP header with output dimensions
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

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = effectiveSrcW;
  ctx.srcHeight = effectiveSrcH;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = geometry.scaleX_fp;
  ctx.scaleY_fp = geometry.scaleY_fp;
  ctx.srcXOffset_fp = geometry.srcXOffset_fp;
  ctx.srcYOffset_fp = geometry.srcYOffset_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.error = false;

  const size_t mcuBufBytes = static_cast<size_t>(MAX_MCU_HEIGHT) * effectiveSrcW;
  size_t scratchBytes = mcuBufBytes + static_cast<size_t>(bytesPerRow);
  if (smoothUpscale) {
    scratchBytes += static_cast<size_t>(outWidth) * 3;
  } else if (needsScaling) {
    scratchBytes += static_cast<size_t>(outWidth) * sizeof(uint32_t) * 2;
  }
  // Keep the conversion in one arena slab. Growing would request another full slab,
  // which can fail on a fragmented heap even when the next buffer is tiny.
  scratchBytes += 128;

  Arena scratchArena;
  if (!scratchArena.init(std::max<size_t>(4096, scratchBytes))) {
    LOG_ERR("JPG", "OOM: JPEG BMP scratch arena (%u bytes)", static_cast<unsigned>(scratchBytes));
    return false;
  }

  // MCU row buffer: MAX_MCU_HEIGHT rows × ctx.srcWidth columns of grayscale.
  ctx.mcuBuf = arenaNewArray<uint8_t>(scratchArena, mcuBufBytes);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%u bytes)", static_cast<unsigned>(mcuBufBytes));
    return false;
  }
  memset(ctx.mcuBuf, 0, mcuBufBytes);

  ctx.bmpRow = arenaNewArray<uint8_t>(scratchArena, bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  if (smoothUpscale) {
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = arenaNewArray<uint8_t>(scratchArena, smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows;
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = arenaNewArray<uint32_t>(scratchArena, outWidth);
    ctx.rowCount = arenaNewArray<uint32_t>(scratchArena, outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = geometry.srcYOffset_fp + geometry.scaleY_fp;
  }

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->isValid()) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth, imageLevels);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth, imageLevels);
      if (!ctx.fsDitherer || !ctx.fsDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        return false;
      }
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, decodeFlags);

  if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
    finishSmoothUpscale(&ctx);
  }

  if (rc != 1 || ctx.error) {
    // Include the image shape so a one-off decode failure can be triaged from
    // serial without pulling the file: progressive vs baseline distinguishes a
    // 1/8-scale progressive-path failure from a baseline/MCU-skip one, and the
    // dimensions flag oversized/odd-subsampling sources.
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d, %dx%d, %s)", rc, jpeg->getLastError(), srcWidth, srcHeight,
            progressive ? "progressive" : "baseline");
    return false;
  }

  if (ctx.needsScaling && ctx.currentOutY < ctx.outHeight) {
    LOG_ERR("JPG", "JPEG decode incomplete: %d/%d output rows written", ctx.currentOutY, ctx.outHeight);
    return false;
  }

  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop, bool imageLevels) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop, false, imageLevels);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight, bool adaptiveContain) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true, adaptiveContain);
}
