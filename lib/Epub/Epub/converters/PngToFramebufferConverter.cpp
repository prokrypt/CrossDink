#include "PngToFramebufferConverter.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PngRowDecoder.h>

#include <cstdlib>
#include <new>

#include "DecodePipeline.h"
#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Per-decode state for the row handler, so no global mutable state is needed.
struct PngContext {
  PngRowDecoder* png{nullptr};
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastDstY{-1};  // Track last rendered destination Y to avoid duplicates

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
  uint32_t lastYieldMs{0};

  // Split decode: the row decoder inflates and converts rows to gray on the
  // worker core, tracking its own last row; this task dithers them.
  DecodePipeline* pipeline{nullptr};
  int workerLastDstY{-1};
};

// Row decoder working set: ~11 KB inflate state plus a 32 KB window (PSRAM
// when available) and two scanlines. Checked before each decode so a low-heap
// page skips the image instead of failing mid-way.
constexpr uint32_t PNG_DECODER_APPROX_SIZE = 44U * 1024U;

// The converter expands each source row to 8-bit gray before dithering; this
// caps that scratch row (the limit PNGdec's 16416-byte buffer used to imply).
constexpr size_t MAX_GRAY_LINE_BUFFER_BYTES = 8208;

bool isSupportedBitDepth(int pixelType, int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PngRowDecoder::Gray || pixelType == PngRowDecoder::Palette;
}

uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  return PngRowDecoder::sample(pixels, static_cast<uint32_t>(x), static_cast<uint8_t>(bitsPerSample));
}

uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  return PngRowDecoder::sampleToByte(sample, static_cast<uint8_t>(bitsPerSample));
}

// Convert entire source line to grayscale with alpha blending to white background.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       const uint8_t* palette, bool hasAlpha) {
  switch (pixelType) {
    case PngRowDecoder::Gray:
      if (bitsPerSample == 8) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PngRowDecoder::Rgb:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PngRowDecoder::Palette:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            const uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            const uint8_t* p = &palette[idx * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PngRowDecoder::GrayAlpha:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PngRowDecoder::Rgba:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

// Maps source row srcY to the output rows it fills after lastDstY. False when
// it fills none.
bool pngOutputRows(const PngContext& ctx, const int srcY, const int lastDstY, int& firstDstY, int& endDstY) {
  // Map source rows with the exact output-height ratio. During downscaling,
  // multiple source rows can select the same output row; during upscaling, one
  // source row must be repeated across every output row in its range. Emitting
  // only the first row of an upscale leaves zero-filled (black) gaps in the
  // streamed pixel cache.
  firstDstY = (srcY * ctx.dstHeight) / ctx.srcHeight;
  endDstY = firstDstY + 1;
  if (ctx.dstHeight > ctx.srcHeight) {
    endDstY = ((srcY + 1) * ctx.dstHeight) / ctx.srcHeight;
  }

  if (firstDstY <= lastDstY) firstDstY = lastDstY + 1;
  if (firstDstY >= endDstY || firstDstY >= ctx.dstHeight) return false;
  if (endDstY > ctx.dstHeight) endDstY = ctx.dstHeight;
  return true;
}

void pngDrawRows(PngContext* ctx, const int firstDstY, const int endDstY, const uint8_t* grayLine) {
  const int srcWidth = ctx->srcWidth;
  // Render scaled rows using Bresenham-style integer stepping (no floating-point division)
  int dstWidth = ctx->dstWidth;
  int outXBase = ctx->config->x;
  int screenWidth = ctx->screenWidth;
  bool useDithering = ctx->config->useDithering;

  // Pre-compute orientation and render-mode state once per callback.
  DirectPixelWriter pw;
  pw.init(*ctx->renderer);

  for (int dstY = firstDstY; dstY < endDstY; dstY++) {
    ctx->lastDstY = dstY;
    int outY = ctx->config->y + dstY;
    if (outY >= ctx->screenHeight) continue;

    pw.beginRow(outY);

    // The cache streams to disk one row at a time. Flushing rows below this one
    // (scanlines arrive top to bottom) repositions the single-row band.
    // A flush failure stops caching for the rest of the decode so we never write
    // past the band buffer; finalize() then drops the partial file.
    bool caching = ctx->caching;
    DirectCacheWriter cw;
    if (caching) {
      if (!ctx->cache.advanceTo(dstY)) {
        caching = false;
        ctx->caching = false;
      } else {
        cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
        cw.beginRow(outY, ctx->config->y + ctx->cache.bandStart);
      }
    }

    int srcX = 0;
    int error = 0;

    for (int dstX = 0; dstX < dstWidth; dstX++) {
      int outX = outXBase + dstX;
      if (outX >= 0 && outX < screenWidth) {
        uint8_t gray = grayLine[srcX];

        uint8_t ditheredGray;
        if (useDithering) {
          ditheredGray = applyBayerDither4Level(gray, outX, outY);
        } else {
          ditheredGray = quantizeGrayTo4Level(gray);
        }
        pw.writePixel(outX, ditheredGray);
        if (caching) cw.writePixel(outX, ditheredGray);
      }

      // Bresenham-style stepping: advance srcX based on ratio srcWidth/dstWidth
      error += srcWidth;
      while (error >= dstWidth) {
        error -= dstWidth;
        srcX++;
      }
    }
  }
}

// Handle one decoded source row. Returns false to stop decoding.
bool pngHandleRow(PngContext* ctx, const int srcY, const uint8_t* pixels) {
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return false;
  const PngRowDecoder& png = *ctx->png;

  int firstDstY = 0;
  int endDstY = 0;
  if (ctx->pipeline) {
    // Worker core. The caller repeats this mapping against its own lastDstY
    // and gets the same rows, since it sees every row this side sends.
    if (!pngOutputRows(*ctx, srcY, ctx->workerLastDstY, firstDstY, endDstY)) return true;
    ctx->workerLastDstY = endDstY - 1;
    uint8_t* slot = ctx->pipeline->acquire();
    if (!slot) return false;
    convertLineToGray(pixels, slot, ctx->srcWidth, png.colorType(), png.bitDepth(), png.palette(), png.hasAlpha());
    DecodePipeline::Block block;
    block.y = srcY;
    block.width = ctx->srcWidth;
    block.widthUsed = ctx->srcWidth;
    block.height = 1;
    ctx->pipeline->commit(block);
    return true;
  }

  if (ctx->config->cancel && ctx->config->cancel->load(std::memory_order_relaxed)) return false;
  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);
  if (!pngOutputRows(*ctx, srcY, ctx->lastDstY, firstDstY, endDstY)) return true;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pixels, ctx->grayLineBuffer, ctx->srcWidth, png.colorType(), png.bitDepth(), png.palette(),
                    png.hasAlpha());
  pngDrawRows(ctx, firstDstY, endDstY, ctx->grayLineBuffer);
  return true;
}

// Decode every row through pngHandleRow. Returns 0 on success, -1 on a
// corrupt stream, 1 when the handler stopped early (cancel, pipeline abort).
int decodePngRows(PngContext* ctx) {
  for (uint32_t y = 0; y < ctx->png->height(); ++y) {
    const uint8_t* row = ctx->png->nextRow();
    if (!row) return -1;
    if (!pngHandleRow(ctx, static_cast<int>(y), row)) return 1;
  }
  return 0;
}

bool drawPipelinedPngRow(void* context, const DecodePipeline::Block& block) {
  auto* ctx = static_cast<PngContext*>(context);
  ImageToFramebufferDecoder::yieldDuringDecode(ctx->lastYieldMs);
  int firstDstY = 0;
  int endDstY = 0;
  if (pngOutputRows(*ctx, block.y, ctx->lastDstY, firstDstY, endDstY)) {
    pngDrawRows(ctx, firstDstY, endDstY, block.pixels);
  }
  return true;
}

int runPngDecode(void* context) { return decodePngRows(static_cast<PngContext*>(context)); }

}  // namespace

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  FsFile file;
  if (!Storage.openFileForRead("PNG", imagePath, file)) return false;
  PngRowDecoder png;
  const bool opened = png.openFile(file);
  file.close();
  if (!opened) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %s", imagePath.c_str());
    return false;
  }
  out.width = static_cast<int>(png.width());
  out.height = static_cast<int>(png.height());
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  if (!MemoryBudget::hasHeapForImageDecoder("PNG", "PNG", PNG_DECODER_APPROX_SIZE)) {
    return false;
  }

  FsFile file;
  PngRowDecoder png;
  bool opened = false;
  if (config.sourceData) {
    opened = png.openMemory(config.sourceData, config.sourceSize);
  } else if (Storage.openFileForRead("PNG", imagePath, file)) {
    opened = png.openFile(file);
  }
  if (!opened) {
    LOG_ERR("PNG", "Failed to open PNG: %s", imagePath.c_str());
    if (file) file.close();
    return false;
  }

  PngContext ctx;
  ctx.png = &png;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  if (!validateImageDimensions(static_cast<int>(png.width()), static_cast<int>(png.height()), "PNG")) {
    if (file) file.close();
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = static_cast<int>(png.width());
  ctx.srcHeight = static_cast<int>(png.height());

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastDstY = -1;  // Reset row tracking

  const int pixelType = png.colorType();
  const int bitsPerSample = png.bitDepth();
  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature(
        "bit depth (" + std::to_string(bitsPerSample) + "bpp) for pixel type " + std::to_string(pixelType), imagePath);
    if (file) file.close();
    return false;
  }

  // The converter expands each source row to 8-bit grayscale before dithering,
  // so this scratch buffer is sized by source pixels even when the decoder
  // reads a packed 1/2/4-bit row.
  const size_t grayBufSize = static_cast<size_t>(ctx.srcWidth);
  if (grayBufSize > MAX_GRAY_LINE_BUFFER_BYTES) {
    LOG_ERR("PNG", "Expanded gray row too wide: need %u bytes for width=%d, max=%u", static_cast<unsigned>(grayBufSize),
            ctx.srcWidth, static_cast<unsigned>(MAX_GRAY_LINE_BUFFER_BYTES));
    if (file) file.close();
    return false;
  }

  if (!png.begin()) {
    if (file) file.close();
    return false;
  }

  auto grayLineBuffer = makeUniqueNoThrow<uint8_t[]>(grayBufSize);
  if (!grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    if (file) file.close();
    return false;
  }
  ctx.grayLineBuffer = grayLineBuffer.get();

  // Stream the pixel cache to disk. Source scanlines arrive top to bottom and
  // we emit at most one (downscaled) output row per source row, so the band
  // only needs a single row. Streaming keeps the working set tiny, so unlike
  // the old full-image buffer it neither competes with the decoder nor forces
  // larger images to skip caching - which previously meant a full re-decode on
  // every one of an image page's ~14 render passes.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  ctx.lastYieldMs = millis();
  // Inflate and row conversion on the worker core while this task dithers the
  // previous rows; the split falls back to an inline decode.
  DecodePipeline pipeline;
  int rc = 0;
  bool decoded = false;
  if (DecodePipeline::worthSplitting() && pipeline.begin(static_cast<size_t>(ctx.srcWidth))) {
    ctx.pipeline = &pipeline;
    decoded = pipeline.run(runPngDecode, &ctx, drawPipelinedPngRow, &ctx, rc);
    ctx.pipeline = nullptr;
  }
  if (!decoded) rc = decodePngRows(&ctx);

  ctx.grayLineBuffer = nullptr;
  if (file) file.close();

  if (rc != 0) {
    LOG_ERR("PNG", "Decode failed: %d", rc);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
