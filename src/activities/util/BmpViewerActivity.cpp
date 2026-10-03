#include "BmpViewerActivity.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <SdCardFontSystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/DecodePipeline.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "Epub/converters/PngToFramebufferConverter.h"
#include "activities/boot_sleep/ImageFolderIndex.h"
#include "activities/home/BookActions.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

bool isViewableImageFile(const std::string& filename) {
  return FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename);
}

// Settings > Display > Image Viewer as a DirectPixelWriter BW look; Same as reader follows
// the reader's Images choice (Gray for Placeholder/Suppress).
uint8_t viewerBwImages() {
  switch (SETTINGS.imageViewerMode) {
    case CrossPointSettings::IMAGE_VIEWER_BW_DARK:
      return DirectPixelWriter::BW_IMAGES_DARK;
    case CrossPointSettings::IMAGE_VIEWER_BW:
      return DirectPixelWriter::BW_IMAGES_BW;
    case CrossPointSettings::IMAGE_VIEWER_DITHER:
      return DirectPixelWriter::BW_IMAGES_DITHER;
    case CrossPointSettings::IMAGE_VIEWER_GRAY:
      return DirectPixelWriter::BW_IMAGES_OFF;
    default:
      break;
  }
  switch (SETTINGS.imageRendering) {
    case CrossPointSettings::IMAGES_DISPLAY_BW_DARK:
      return DirectPixelWriter::BW_IMAGES_DARK;
    case CrossPointSettings::IMAGES_DISPLAY_BW:
      return DirectPixelWriter::BW_IMAGES_BW;
    case CrossPointSettings::IMAGES_DISPLAY_DITHER:
      return DirectPixelWriter::BW_IMAGES_DITHER;
    default:
      return DirectPixelWriter::BW_IMAGES_OFF;
  }
}

// Centers the PNG on the page, scaled down to fit. BW and BW dark threshold with no ordered dither;
// Dither and Gray keep the PNG decoder's ordered dither.
bool pngRenderConfig(const std::string& path, const int pageWidth, const int pageHeight, const uint8_t look,
                     RenderConfig& config) {
  ImageDimensions dims;
  if (!PngToFramebufferConverter::getDimensionsStatic(path, dims)) return false;
  float scale = 1.0f;
  if (dims.width > pageWidth || dims.height > pageHeight) {
    const float scaleX = static_cast<float>(pageWidth) / static_cast<float>(dims.width);
    const float scaleY = static_cast<float>(pageHeight) / static_cast<float>(dims.height);
    scale = std::min(scaleX, scaleY);
  }
  config.maxWidth = std::max(1, static_cast<int>(static_cast<float>(dims.width) * scale));
  config.maxHeight = std::max(1, static_cast<int>(static_cast<float>(dims.height) * scale));
  config.x = (pageWidth - config.maxWidth) / 2;
  config.y = (pageHeight - config.maxHeight) / 2;
  config.useGrayscale = true;
  config.useDithering = look != DirectPixelWriter::BW_IMAGES_BW && look != DirectPixelWriter::BW_IMAGES_DARK;
  config.performanceMode = false;
  config.useExactDimensions = true;
  return true;
}

// Centers the BMP on the page; scale < 1 when it is shrunk to fit (as GfxRenderer::drawBitmap does).
void fitBitmap(const Bitmap& bitmap, const int pageWidth, const int pageHeight, int& x, int& y, float& scale) {
  scale = 1.0f;
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    const float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);
    if (ratio > screenRatio) {
      // Wider than screen
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      // Taller than screen
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
    scale = std::min(static_cast<float>(pageWidth) / bitmap.getWidth(),
                     static_cast<float>(pageHeight) / bitmap.getHeight());
  } else {
    // Center small images
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }
}

// Decode-once BMP: the worker core reads (and dithers) rows into pipeline slots; the render task
// writes their levels through DirectPixelWriter's level planes.
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
  int x, y, screenWidth, screenHeight;
  float scale;
  const std::atomic<bool>* cancel;
};

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

// Same placement as GfxRenderer::drawBitmap (no crop).
void writeBmpLevelRow(BmpLevelSink& sink, const uint8_t* row, const int bmpY) {
  const Bitmap& bitmap = *sink.bitmap;
  int screenY = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
  if (sink.scale < 1.0f) screenY = static_cast<int>(std::floor(screenY * sink.scale));
  screenY += sink.y;
  if (screenY < 0 || screenY >= sink.screenHeight) return;
  sink.pw.beginRow(screenY);
  for (int bmpX = 0; bmpX < bitmap.getWidth(); bmpX++) {
    int screenX = sink.scale < 1.0f ? static_cast<int>(std::floor(bmpX * sink.scale)) : bmpX;
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
    if (!slot) return 0;  // stopped by the render task
    if (job.bitmap->readNextRow(slot, job.fileRow) != BmpReaderError::Ok) return -1;
    DecodePipeline::Block block;
    block.y = bmpY;
    block.width = job.bitmap->getWidth();
    block.height = 1;
    job.pipeline->commit(block);
  }
  return 0;
}

bool drawBmpLevelRow(void* context, const DecodePipeline::Block& block) {  // render task
  auto& sink = *static_cast<BmpLevelSink*>(context);
  writeBmpLevelRow(sink, block.pixels, block.y);
  return !sink.cancel->load(std::memory_order_acquire);
}

bool isMacOSSidecarFile(const std::string& filename) { return filename.rfind("._", 0) == 0; }

std::string imageDisplayName(const std::string& path) {
  const size_t filenameStart = path.find_last_of('/') + 1;
  const size_t extensionStart = path.find_last_of('.');
  return path.substr(filenameStart, extensionStart - filenameStart);
}

void drawImageError(GfxRenderer& renderer, const MappedInputManager& mappedInput, const char* message) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, message);
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("BmpViewer", renderer, mappedInput), filePath(std::move(path)) {}

void BmpViewerActivity::loadSiblingImages() {
  siblingImages.clear();
  currentImageIndex = -1;

  if (filePath.empty()) return;

  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  size_t lastSlash = filePath.find_last_of('/');
  std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;

  auto dir = Storage.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }

  char name[500];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (!file.isDirectory()) {
      file.getName(name, sizeof(name));
      if (name[0] != '.' && !isMacOSSidecarFile(name)) {
        std::string fname(name);
        if (isViewableImageFile(fname)) {
          siblingImages.push_back(fname);
        }
      }
    }
    file.close();
  }
  dir.close();

  FsHelpers::sortFileList(siblingImages);

  const auto match = std::find(siblingImages.begin(), siblingImages.end(), fileName);
  if (match != siblingImages.end()) currentImageIndex = static_cast<int>(match - siblingImages.begin());
}

bool BmpViewerActivity::renderPngImage() {
  const uint8_t bw = viewerBwImages();
  RenderConfig config;
  if (!pngRenderConfig(filePath, renderer.getScreenWidth(), renderer.getScreenHeight(), bw, config)) {
    drawImageError(renderer, mappedInput, "Invalid PNG File");
    return false;
  }
  const int x = config.x, y = config.y, drawWidth = config.maxWidth, drawHeight = config.maxHeight;

  PngToFramebufferConverter converter;
  DirectPixelWriter::bwImages = bw;
  const bool success = showImage(!bw && renderer.supportsAbsoluteGrayscale(), [&]() {
    if (!converter.decodeToFramebuffer(filePath, renderer, config)) return false;
    renderer.preserveImagePolarity(x, y, drawWidth, drawHeight);
    return true;
  });
  DirectPixelWriter::bwImages = DirectPixelWriter::BW_IMAGES_OFF;
  if (!success) {
    LOG_ERR("BMP", "Failed to render PNG image");
    drawImageError(renderer, mappedInput, "Invalid PNG File");
  }
  return success;
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  if (siblingImages.empty() && !filePath.empty()) {
    loadSiblingImages();
  }
  requestImageRedraw();
}

void BmpViewerActivity::requestImageRedraw() {
  drawCancelled.store(false, std::memory_order_release);
  needsImageRedraw.store(true, std::memory_order_release);
  requestUpdate();
}

void BmpViewerActivity::onFrontlightPanelClosed() {
  // The drop-down panel drew over the top of the image in the shared framebuffer.
  requestImageRedraw();
}

void BmpViewerActivity::render(RenderLock&&) {
  if (!needsImageRedraw.exchange(false, std::memory_order_acq_rel)) return;
  drawImage();
}

void BmpViewerActivity::drawImage() {
  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  // The popup alone: each progress step was another ~620 ms FAST refresh before the decode.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  // Decode once into PSRAM planes (or reuse a recent decode). Without them, or when the decode
  // fails, the per-pass draw below runs and reports the error.
  if (renderer.supportsDirectGrayscale() && psramHeapAvailable()) {
    const DecodedImage* image = decodedImage(viewerBwImages());
    if (drawCancelled.load(std::memory_order_acquire)) return;  // the next screen repaints over the popup
    if (image) {
      if (!showDecoded(*image)) {
        LOG_ERR("BMP", "Failed to show decoded image");
        drawImageError(renderer, mappedInput, tr(STR_FAILED_LOWER));
      }
      return;
    }
  }

  if (FsHelpers::hasPngExtension(filePath)) {
    renderPngImage();
    return;
  }

  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    // BW looks come out 0/3 from the reader and skip the gray passes: one FAST refresh.
    const uint8_t bw = viewerBwImages();
    Bitmap bitmap(file, bw != DirectPixelWriter::BW_IMAGES_BW && bw != DirectPixelWriter::BW_IMAGES_DARK,
                  renderer.supportsAbsoluteGrayscale());
    if (bw) bitmap.setBwOutput(bw == DirectPixelWriter::BW_IMAGES_DARK ? 192 : 128);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;
      float scale;
      fitBitmap(bitmap, pageWidth, pageHeight, x, y, scale);

      const bool success = showImage(!bw && bitmap.hasGreyscale() && renderer.supportsAbsoluteGrayscale(), [&]() {
        return bitmap.rewindToData() == BmpReaderError::Ok && renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight);
      });
      if (!success) {
        LOG_ERR("BMP", "Failed to render complete BMP image");
        drawImageError(renderer, mappedInput, tr(STR_FAILED_LOWER));
      }

    } else {
      // Handle file parsing error
      drawImageError(renderer, mappedInput, "Invalid BMP File");
    }

    file.close();
  } else {
    // Handle file open error
    drawImageError(renderer, mappedInput, "Could not open file");
  }
}

// Draws the image with drawFrame() (which re-reads it from the start each call) and shows it:
// gray passes when gray, else one FAST refresh. False when a draw failed.
bool BmpViewerActivity::showImage(const bool gray, const std::function<bool()>& drawImageFrame) {
  const auto drawFrame = [&]() {
    if (!drawImageFrame()) return false;
    drawHints();
    return true;
  };
  // PSRAM boards with Direct gray (X4 Pro) show the image once, as the sleep screen does: both
  // gray planes render into PSRAM, then one direct-gray refresh, instead of a B/W image on a
  // full refresh followed by the gray pass. Nothing reaches the panel until the planes are
  // done, so input can drop the draw between decodes with no panel state to undo.
  HeapByteBuffer lsbPlane;
  HeapByteBuffer msbPlane;
  if (gray && renderer.supportsDirectGrayscale() && psramHeapAvailable()) {
    const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
    lsbPlane = makePsramByteBufferNoThrow(planeBytes);
    if (lsbPlane) msbPlane = makePsramByteBufferNoThrow(planeBytes);
  }
  bool success = true;
  if (msbPlane) {
    const auto cancelled = [this]() { return drawCancelled.load(std::memory_order_acquire); };
    renderer.setAbsoluteGrayPlanes(true);  // Direct takes complete planes
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (!success || cancelled()) break;
      renderer.setRenderMode(mode);
      renderer.beginStripTarget(mode == GfxRenderer::GRAYSCALE_LSB ? lsbPlane.get() : msbPlane.get(), 0,
                                renderer.getDisplayHeight());
      renderer.clearScreen();
      success = drawFrame();
      renderer.endStripTarget();
    }
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.setAbsoluteGrayPlanes(false);
    if (cancelled()) return true;  // the panel still shows the popup; the next screen repaints over it
    success = success && renderer.displayDirectGrayscaleBase();
    if (success) {
      renderer.copyGrayscalePlanes(lsbPlane.get(), msbPlane.get());
      renderer.displayGrayBuffer();
      // Popups and the cleanup need the B/W image.
      renderer.clearScreen();
      success = drawFrame();
      if (success) renderer.cleanupGrayscaleWithFrameBuffer();
    }
  } else {
    renderer.clearScreen();
    success = drawFrame();
  }
  if (!msbPlane && success && gray) {
    success = renderer.displayAbsoluteGrayscaleBase();
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      if (!success) break;
      renderer.clearScreen();
      renderer.setRenderMode(mode);
      success = drawFrame();
      if (!success) break;
      if (mode == GfxRenderer::GRAYSCALE_LSB)
        renderer.copyGrayscaleLsbBuffers();
      else
        renderer.copyGrayscaleMsbBuffers();
    }
    if (success) renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
    // Popups need the original B/W image, not the last gray selector plane.
    if (success) {
      renderer.clearScreen();
      success = drawFrame();
      if (success) renderer.cleanupGrayscaleWithFrameBuffer();
    }
  } else if (!msbPlane && success) {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  return success;
}

const BmpViewerActivity::DecodedImage* BmpViewerActivity::decodedImage(const uint8_t look) {
  for (auto it = recentImages.begin(); it != recentImages.end(); ++it) {
    if (it->path == filePath && it->look == look) {
      std::rotate(it, it + 1, recentImages.end());  // newest last
      return &recentImages.back();
    }
  }
  constexpr size_t kRecentImages = 4;  // ponytail: linear scan; 4 x 96 KB of PSRAM
  if (recentImages.size() >= kRecentImages) recentImages.erase(recentImages.begin());

  DecodedImage image;
  image.path = filePath;
  image.look = look;
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
  image.lsb = makePsramByteBufferNoThrow(planeBytes);
  if (image.lsb) image.msb = makePsramByteBufferNoThrow(planeBytes);
  if (!image.msb) {
    LOG_ERR("BMP", "No PSRAM for decoded planes (%u B)", static_cast<unsigned>(2 * planeBytes));
    return nullptr;
  }
  memset(image.lsb.get(), 0xFF, planeBytes);  // white
  memset(image.msb.get(), 0xFF, planeBytes);

  const uint32_t startedAt = millis();
  DirectPixelWriter::levelPlanes[0] = image.lsb.get();
  DirectPixelWriter::levelPlanes[1] = image.msb.get();
  DirectPixelWriter::bwImages = look;
  const bool decoded = FsHelpers::hasPngExtension(filePath) ? decodePngLevels(image) : decodeBmpLevels(image);
  DirectPixelWriter::levelPlanes[0] = DirectPixelWriter::levelPlanes[1] = nullptr;
  DirectPixelWriter::bwImages = DirectPixelWriter::BW_IMAGES_OFF;
  if (!decoded || drawCancelled.load(std::memory_order_acquire)) return nullptr;
  LOG_INF("BMP", "Decoded once in %u ms", static_cast<unsigned>(millis() - startedAt));
  recentImages.push_back(std::move(image));
  return &recentImages.back();
}

bool BmpViewerActivity::decodePngLevels(DecodedImage& image) {
  RenderConfig config;
  if (!pngRenderConfig(filePath, renderer.getScreenWidth(), renderer.getScreenHeight(), image.look, config)) {
    return false;
  }
  config.cancel = &drawCancelled;
  image.gray = !image.look;
  image.x = config.x;
  image.y = config.y;
  image.width = config.maxWidth;
  image.height = config.maxHeight;
  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(filePath, renderer, config);
}

bool BmpViewerActivity::decodeBmpLevels(DecodedImage& image) {
  HalFile file;
  if (!Storage.openFileForRead("BMP", filePath, file)) return false;
  const uint8_t look = image.look;
  Bitmap bitmap(file, look != DirectPixelWriter::BW_IMAGES_BW && look != DirectPixelWriter::BW_IMAGES_DARK,
                renderer.supportsAbsoluteGrayscale());
  if (look) bitmap.setBwOutput(look == DirectPixelWriter::BW_IMAGES_DARK ? 192 : 128);
  bool ok = bitmap.parseHeaders() == BmpReaderError::Ok;
  if (ok) {
    BmpLevelSink sink;
    sink.pw.init(renderer);
    sink.bitmap = &bitmap;
    sink.screenWidth = renderer.getScreenWidth();
    sink.screenHeight = renderer.getScreenHeight();
    sink.cancel = &drawCancelled;
    fitBitmap(bitmap, sink.screenWidth, sink.screenHeight, sink.x, sink.y, sink.scale);
    image.gray = !look && bitmap.hasGreyscale();
    image.x = sink.x;
    image.y = sink.y;
    image.width =
        sink.scale < 1.0f ? static_cast<int>(std::floor((bitmap.getWidth() - 1) * sink.scale)) + 1 : bitmap.getWidth();
    image.height = sink.scale < 1.0f ? static_cast<int>(std::floor((bitmap.getHeight() - 1) * sink.scale)) + 1
                                     : bitmap.getHeight();

    // ponytail: files over 4 MB of pixels stream from the card rather than take that much PSRAM.
    HeapByteBuffer pixels;
    if (bitmap.pixelDataBytes() <= 4u * 1024 * 1024) pixels = makePsramByteBufferNoThrow(bitmap.pixelDataBytes());
    HeapByteBuffer fileRow = makePsramByteBufferNoThrow(bitmap.getRowBytes());
    const size_t levelRowBytes = (bitmap.getWidth() + 3) / 4;
    BmpLevelJob job{&bitmap, &file, pixels.get(), fileRow.get(), nullptr};
    DecodePipeline pipeline;
    int rc = 0;
    bool split = false;
    ok = fileRow != nullptr;
    if (ok && DecodePipeline::worthSplitting() && pipeline.begin(levelRowBytes)) {
      job.pipeline = &pipeline;
      split = pipeline.run(readBmpLevelRows, &job, drawBmpLevelRow, &sink, rc);
      ok = !split || rc == 0;
    }
    if (ok && !split) {
      HeapByteBuffer levelRow = makePsramByteBufferNoThrow(levelRowBytes);
      ok = levelRow != nullptr;
      if (ok) loadBmpPixels(job);
      for (int bmpY = 0; ok && bmpY < bitmap.getHeight() && !drawCancelled.load(std::memory_order_acquire); bmpY++) {
        ok = bitmap.readNextRow(levelRow.get(), fileRow.get()) == BmpReaderError::Ok;
        if (ok) writeBmpLevelRow(sink, levelRow.get(), bmpY);
      }
    }
  }
  file.close();
  return ok;
}

bool BmpViewerActivity::showDecoded(const DecodedImage& image) {
  const size_t planeBytes = static_cast<size_t>(renderer.getDisplayWidthBytes()) * renderer.getDisplayHeight();
  if (image.gray) {
    // Working copies take the button hints; the recent copy stays clean.
    HeapByteBuffer lsb = makePsramByteBufferNoThrow(planeBytes);
    HeapByteBuffer msb;
    if (lsb) msb = makePsramByteBufferNoThrow(planeBytes);
    if (!msb) return false;
    memcpy(lsb.get(), image.lsb.get(), planeBytes);
    memcpy(msb.get(), image.msb.get(), planeBytes);
    renderer.setAbsoluteGrayPlanes(true);  // Direct takes complete planes
    for (const auto mode : {GfxRenderer::GRAYSCALE_LSB, GfxRenderer::GRAYSCALE_MSB}) {
      renderer.setRenderMode(mode);
      renderer.beginStripTarget(mode == GfxRenderer::GRAYSCALE_LSB ? lsb.get() : msb.get(), 0,
                                renderer.getDisplayHeight());
      drawHints();
      renderer.endStripTarget();
    }
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.setAbsoluteGrayPlanes(false);
    if (!renderer.displayDirectGrayscaleBase()) return false;
    renderer.copyGrayscalePlanes(lsb.get(), msb.get());
    renderer.displayGrayBuffer();
  }
  // The B/W image for popups and the gray cleanup: white only where both bits are set (level 3).
  uint8_t* frame = renderer.getFrameBuffer();
  for (size_t i = 0; i < planeBytes; i++) frame[i] = image.lsb[i] & image.msb[i];
  renderer.preserveImagePolarity(image.x, image.y, image.width, image.height);
  drawHints();
  if (image.gray) {
    renderer.cleanupGrayscaleWithFrameBuffer();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
  return true;
}

void BmpViewerActivity::drawHints() const {
  const bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
  const bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                        currentImageIndex < static_cast<int>(siblingImages.size()) - 1);
  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SET_SLEEP_COVER),
                                            (hasPrevious ? "<" : nullptr), (hasNext ? ">" : nullptr));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  recentImages.clear();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  // The popups draw from this task; wait for any image redraw on the render task.
  RenderLock lock(*this);
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  APP_STATE.favoriteSleepImagePath = filePath;
  if (APP_STATE.saveToFile()) {
    LOG_INF("BmpViewer", "Pinned favorite sleep image: %s", filePath.c_str());

    // PNG covers only render in Page Overlay sleep mode; other modes silently skip them (see
    // selectPinnedSleepImage() in SleepActivity.cpp), so switch the setting for the user.
    if (FsHelpers::hasPngExtension(filePath) &&
        SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY) {
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY;
      if (SETTINGS.saveToFile()) {
        LOG_INF("BmpViewer", "Switched sleep wallpaper mode to Page Overlay for PNG cover");
      } else {
        LOG_ERR("BmpViewer", "Failed to save sleep wallpaper mode after setting PNG cover");
      }
    }

    GUI.drawPopup(renderer, tr(STR_DONE));
  } else {
    LOG_ERR("BmpViewer", "Failed to save favorite sleep image path: %s", filePath.c_str());
    GUI.drawPopup(renderer, tr(STR_FAILED_LOWER));
  }
  lock.unlock();

  delay(1000);
  requestImageRedraw();
}

void BmpViewerActivity::pinSleepFavorite() {
  APP_STATE.favoriteSleepImagePath = filePath;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BmpViewer", "Failed to save favorite sleep image path: %s", filePath.c_str());
    return;
  }
  LOG_INF("BmpViewer", "Pinned favorite sleep image: %s", filePath.c_str());

  // Keep the context-menu action consistent with Confirm: PNG sleep images
  // only render in Page Overlay mode.
  if (FsHelpers::hasPngExtension(filePath) && SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY) {
    SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY;
    if (!SETTINGS.saveToFile()) {
      LOG_ERR("BmpViewer", "Failed to save Page Overlay mode for PNG sleep image");
    }
  }
}

void BmpViewerActivity::unpinSleepFavorite() {
  APP_STATE.favoriteSleepImagePath.clear();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BmpViewer", "Failed to clear favorite sleep image");
    return;
  }
  LOG_INF("BmpViewer", "Cleared favorite sleep image");
}

void BmpViewerActivity::pinBootFavorite() {
  APP_STATE.favoriteBootImagePath = filePath;
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BmpViewer", "Failed to save favorite boot image path: %s", filePath.c_str());
    return;
  }
  LOG_INF("BmpViewer", "Pinned favorite boot image: %s", filePath.c_str());
}

void BmpViewerActivity::unpinBootFavorite() {
  APP_STATE.favoriteBootImagePath.clear();
  if (!APP_STATE.saveToFile()) {
    LOG_ERR("BmpViewer", "Failed to clear favorite boot image");
    return;
  }
  LOG_INF("BmpViewer", "Cleared favorite boot image");
}

void BmpViewerActivity::promptDeleteImage() {
  const std::string path = filePath;
  needsImageRedraw.store(true, std::memory_order_release);  // the prompt draws over the image
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, BookActions::confirmationHeading(StrId::STR_DELETE),
                                             imageDisplayName(path)),
      [this, path](const ActivityResult& result) {
        if (result.isCancelled) return;
        if (!Storage.remove(path.c_str())) {
          LOG_ERR("BmpViewer", "Failed to delete image: %s", path.c_str());
          return;
        }
        ImageFolderIndex::invalidateForPath(path.c_str());
        sdFontSystem.markRegistryDirtyForPath(path.c_str());
        if (APP_STATE.favoriteSleepImagePath == path) {
          unpinSleepFavorite();
        }
        if (APP_STATE.favoriteBootImagePath == path) {
          unpinBootFavorite();
        }
        activityManager.goToFileBrowser(path);
      });
}

void BmpViewerActivity::showContextMenu() {
  std::vector<FileBrowserActionActivity::MenuItem> items = BookActions::buildBookActionItems(filePath, false);
  if (BookActions::canSendNearby(filePath)) {
    items.push_back({FileBrowserAction::SendNearby, StrId::STR_SEND_NEARBY_BOOK});
  }

  const bool isPinned = APP_STATE.favoriteSleepImagePath == filePath;
  items.push_back({isPinned ? FileBrowserAction::UnpinFavorite : FileBrowserAction::PinFavorite,
                   isPinned ? StrId::STR_UNPIN_AS_FAVORITE : StrId::STR_PIN_AS_FAVORITE});

  if (FsHelpers::hasBmpExtension(filePath)) {
    const bool isBootPinned = APP_STATE.favoriteBootImagePath == filePath;
    items.push_back({isBootPinned ? FileBrowserAction::UnpinBootFavorite : FileBrowserAction::PinBootFavorite,
                     isBootPinned ? StrId::STR_CLEAR_BOOT_SCREEN : StrId::STR_SET_AS_BOOT_SCREEN});
  }

  needsImageRedraw.store(true, std::memory_order_release);  // the menu draws over the image
  startActivityForResult(std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, imageDisplayName(filePath),
                                                                     std::move(items), false, false),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;

                           const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
                           if (actionResult == nullptr) return;

                           switch (static_cast<FileBrowserAction>(actionResult->action)) {
                             case FileBrowserAction::Delete:
                               promptDeleteImage();
                               return;
                             case FileBrowserAction::SendNearby:
                               activityManager.goToNearbyBookSend(filePath, false);
                               return;
                             case FileBrowserAction::PinFavorite:
                               if (FsHelpers::hasPngExtension(filePath)) {
                                 startActivityForResult(std::make_unique<ConfirmationActivity>(
                                                            renderer, mappedInput, "", tr(STR_PIN_PNG_WARNING)),
                                                        [this](const ActivityResult& confirmation) {
                                                          if (!confirmation.isCancelled) pinSleepFavorite();
                                                        });
                               } else {
                                 pinSleepFavorite();
                               }
                               return;
                             case FileBrowserAction::UnpinFavorite:
                               unpinSleepFavorite();
                               return;
                             case FileBrowserAction::PinBootFavorite:
                               pinBootFavorite();
                               return;
                             case FileBrowserAction::UnpinBootFavorite:
                               unpinBootFavorite();
                               return;
                             case FileBrowserAction::DeleteCache:
                             case FileBrowserAction::ReadingStats:
                             case FileBrowserAction::ToggleBookStatsTracking:
                             case FileBrowserAction::SetSleepFolder:
                             case FileBrowserAction::ClearSleepFolder:
                             case FileBrowserAction::ToggleCompleted:
                             case FileBrowserAction::RemoveFromRecents:
                             case FileBrowserAction::DeleteStats:
                             case FileBrowserAction::ViewBookmarks:
                             case FileBrowserAction::ViewClippings:
                             case FileBrowserAction::DeleteBookmarks:
                             case FileBrowserAction::DeleteClippings:
                             case FileBrowserAction::EpubRenderMode:
                             case FileBrowserAction::ResetReaderSettings:
                             case FileBrowserAction::Rename:
                               return;
                           }
                         });
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  // Input that leaves or replaces the image stops a draw in progress before it
  // reaches the panel, so the RenderLock taken below or by the screen change
  // waits at most one decode pass instead of the whole draw.
  const auto cancelDraw = [this]() { drawCancelled.store(true, std::memory_order_release); };

  auto openSibling = [this, &cancelDraw](const int delta) {
    if (currentImageIndex < 0) {
      return false;
    }
    const int nextIndex = currentImageIndex + delta;
    if (siblingImages.size() <= 1 || nextIndex < 0 || nextIndex >= static_cast<int>(siblingImages.size())) {
      return false;
    }
    cancelDraw();
    std::string dirPath = FsHelpers::extractFolderPath(filePath);
    if (dirPath.back() != '/') dirPath += "/";
    {
      // render() reads filePath and currentImageIndex on the render task.
      RenderLock lock(*this);
      currentImageIndex = nextIndex;
      filePath = dirPath + siblingImages[currentImageIndex];
    }
    requestImageRedraw();
    return true;
  };

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancelDraw();
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.hasTouchHardware()) {
    constexpr unsigned long CONTEXT_MENU_HOLD_MS = 1000;
    int touchX = 0;
    int touchY = 0;
    if (mappedInput.isScreenTouchLongPress(touchX, touchY, CONTEXT_MENU_HOLD_MS)) {
      mappedInput.suppressCurrentTouchContact();
      cancelDraw();
      showContextMenu();
      return;
    }
    if (mappedInput.wasScreenTapped(touchX, touchY)) {
      cancelDraw();
      showContextMenu();
      return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Left) {
    openSibling(1);
    return;
  }
  if (swipe == MappedInputManager::SwipeDir::Right) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (isViewableImageFile(filePath)) {
      cancelDraw();
      doSetSleepCover();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    openSibling(-1);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    openSibling(1);
    return;
  }
}
