#include "BmpLevelDecode.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cmath>

#include "Epub/converters/DecodePipeline.h"
#include "Epub/converters/DirectPixelWriter.h"

namespace {

// The worker core reads (and dithers) rows into pipeline slots; the caller writes their levels
// through DirectPixelWriter's level planes.
struct BmpLevelJob {
  Bitmap* bitmap;
  HalFile* file;
  uint8_t* pixels;   // PSRAM copy of the pixel data, or nullptr to read rows from the SD card
  uint8_t* fileRow;  // raw row scratch for Bitmap::readNextRow
  DecodePipeline* pipeline;
};

struct BmpLevelSink {
  DirectPixelWriter pw;
  const Bitmap* bitmap;
  int x, y, cropPixX, cropPixY, screenWidth, screenHeight;
  float scale;
  const std::atomic<bool>* cancel;
};

bool cancelled(const BmpLevelSink& sink) { return sink.cancel && sink.cancel->load(std::memory_order_acquire); }

// One big read instead of a row at a time; on a short read the rows come from the card as before.
void loadBmpPixels(BmpLevelJob& job) {
  if (!job.pixels) return;
  const size_t total = job.bitmap->pixelDataBytes();
  size_t got = 0;
  while (got < total) {
    const int n = job.file->read(job.pixels + got, std::min<size_t>(total - got, 64 * 1024));
    if (n <= 0) break;
    got += static_cast<size_t>(n);
  }
  if (got == total) {
    job.bitmap->setPixelData(job.pixels);
  } else {
    LOG_ERR("BMP", "Pixel preload short (%u of %u B); reading rows", static_cast<unsigned>(got),
            static_cast<unsigned>(total));
    job.bitmap->rewindToData();
  }
}

// Same placement as GfxRenderer::drawBitmap.
void writeBmpLevelRow(BmpLevelSink& sink, const uint8_t* row, const int bmpY) {
  const Bitmap& bitmap = *sink.bitmap;
  if (bmpY < sink.cropPixY || bmpY >= bitmap.getHeight() - sink.cropPixY) return;
  int screenY = -sink.cropPixY + (bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY);
  if (sink.scale < 1.0f) screenY = static_cast<int>(std::floor(screenY * sink.scale));
  screenY += sink.y;
  if (screenY < 0 || screenY >= sink.screenHeight) return;
  sink.pw.beginRow(screenY);
  for (int bmpX = sink.cropPixX; bmpX < bitmap.getWidth() - sink.cropPixX; bmpX++) {
    int screenX = bmpX - sink.cropPixX;
    if (sink.scale < 1.0f) screenX = static_cast<int>(std::floor(screenX * sink.scale));
    screenX += sink.x;
    if (screenX >= sink.screenWidth) break;
    if (screenX < 0) continue;
    sink.pw.writePixel(screenX, (row[bmpX / 4] >> (6 - (bmpX % 4) * 2)) & 0x3);
  }
}

int readBmpLevelRows(void* context) {  // worker core
  auto& job = *static_cast<BmpLevelJob*>(context);
  loadBmpPixels(job);
  for (int bmpY = 0; bmpY < job.bitmap->getHeight(); bmpY++) {
    uint8_t* slot = job.pipeline->acquire();
    if (!slot) return 0;  // stopped by the caller
    if (job.bitmap->readNextRow(slot, job.fileRow) != BmpReaderError::Ok) return -1;
    DecodePipeline::Block block;
    block.y = bmpY;
    block.width = job.bitmap->getWidth();
    block.height = 1;
    job.pipeline->commit(block);
  }
  return 0;
}

bool drawBmpLevelRow(void* context, const DecodePipeline::Block& block) {  // caller
  auto& sink = *static_cast<BmpLevelSink*>(context);
  writeBmpLevelRow(sink, block.pixels, block.y);
  return !cancelled(sink);
}

}  // namespace

bool decodeBmpLevelPlanes(GfxRenderer& renderer, Bitmap& bitmap, HalFile& file, const int x, const int y,
                          const int maxWidth, const int maxHeight, const float cropX, const float cropY,
                          const std::atomic<bool>* cancel) {
  BmpLevelSink sink;
  sink.pw.init(renderer);
  sink.bitmap = &bitmap;
  sink.x = x;
  sink.y = y;
  sink.screenWidth = renderer.getScreenWidth();
  sink.screenHeight = renderer.getScreenHeight();
  sink.cancel = cancel;
  // Same crop and fit scale as GfxRenderer::drawBitmap.
  sink.cropPixX = static_cast<int>(std::floor(bitmap.getWidth() * cropX / 2.0f));
  sink.cropPixY = static_cast<int>(std::floor(bitmap.getHeight() * cropY / 2.0f));
  const float croppedWidth = (1.0f - cropX) * static_cast<float>(bitmap.getWidth());
  const float croppedHeight = (1.0f - cropY) * static_cast<float>(bitmap.getHeight());
  sink.scale = 1.0f;
  if (maxWidth > 0 && croppedWidth > 0.0f) sink.scale = static_cast<float>(maxWidth) / croppedWidth;
  if (maxHeight > 0 && croppedHeight > 0.0f) {
    sink.scale = std::min(sink.scale, static_cast<float>(maxHeight) / croppedHeight);
  }
  sink.scale = std::min(sink.scale, 1.0f);

  // ponytail: files over 4 MB of pixels stream from the card rather than take that much PSRAM.
  HeapByteBuffer pixels;
  if (bitmap.pixelDataBytes() <= 4u * 1024 * 1024) pixels = makePsramByteBufferNoThrow(bitmap.pixelDataBytes());
  HeapByteBuffer fileRow = makePsramByteBufferNoThrow(bitmap.getRowBytes());
  if (!fileRow) {
    LOG_ERR("BMP", "No PSRAM for a %d B row", bitmap.getRowBytes());
    return false;
  }
  const size_t levelRowBytes = (bitmap.getWidth() + 3) / 4;
  BmpLevelJob job{&bitmap, &file, pixels.get(), fileRow.get(), nullptr};
  DecodePipeline pipeline;
  // cppcheck-suppress variableScope ; written through the out-param below
  int rc = 0;
  bool split = false;
  if (DecodePipeline::worthSplitting() && pipeline.begin(levelRowBytes)) {
    job.pipeline = &pipeline;
    split = pipeline.run(readBmpLevelRows, &job, drawBmpLevelRow, &sink, rc);
  }
  bool ok = !split || rc == 0;
  if (!split) {
    HeapByteBuffer levelRow = makePsramByteBufferNoThrow(levelRowBytes);
    ok = levelRow != nullptr;
    if (!ok) LOG_ERR("BMP", "No PSRAM for a %u B level row", static_cast<unsigned>(levelRowBytes));
    if (ok) loadBmpPixels(job);
    for (int bmpY = 0; ok && bmpY < bitmap.getHeight() && !cancelled(sink); bmpY++) {
      ok = bitmap.readNextRow(levelRow.get(), fileRow.get()) == BmpReaderError::Ok;
      if (ok) writeBmpLevelRow(sink, levelRow.get(), bmpY);
    }
  }
  bitmap.setPixelData(nullptr);  // the copy is freed on return
  return ok;
}
