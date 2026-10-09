#include "ScreenshotUtil.h"

#include <Arduino.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <string>

#include "activities/Activity.h"

void ScreenshotUtil::buildFilename(const ScreenshotInfo& info, char* buf, size_t bufSize) {
  const unsigned long ts = millis();

  if (info.readerType == ScreenshotInfo::ReaderType::None || info.title[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.png", ts);
    return;
  }

  char sanitizedTitle[64];
  FsHelpers::sanitizePathComponentForFat32(info.title, sanitizedTitle, sizeof(sanitizedTitle));
  if (sanitizedTitle[0] == '\0') {
    snprintf(buf, bufSize, "/screenshots/screenshot-%lu.png", ts);
    return;
  }

  int pct = info.progressPercent;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  // Display spine index as 1-based for user-facing filenames
  const int chapterNum = info.spineIndex + 1;

  if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
    snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%lu.png", sanitizedTitle, sanitizedTitle, chapterNum,
             info.currentPage, pct, ts);
  } else {
    snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%lu.png", sanitizedTitle, sanitizedTitle, info.currentPage,
             pct, ts);
  }

  // Truncate title if total path exceeds FAT32 limit
  if (strlen(buf) > 255) {
    size_t titleLen = strlen(sanitizedTitle);
    size_t overhead = strlen(buf) - 2 * titleLen;
    if (overhead < 255) {
      size_t maxTitleLen = (255 - overhead) / 2;
      // Walk back to a valid UTF-8 boundary to avoid corrupting multibyte characters
      while (maxTitleLen > 0 && (sanitizedTitle[maxTitleLen] & 0xC0) == 0x80) {
        maxTitleLen--;
      }
      sanitizedTitle[maxTitleLen] = '\0';
      if (info.readerType == ScreenshotInfo::ReaderType::Epub && info.spineIndex >= 0) {
        snprintf(buf, bufSize, "/screenshots/%s/%s_ch%d_p%d_%dpct_%lu.png", sanitizedTitle, sanitizedTitle, chapterNum,
                 info.currentPage, pct, ts);
      } else {
        snprintf(buf, bufSize, "/screenshots/%s/%s_p%d_%dpct_%lu.png", sanitizedTitle, sanitizedTitle, info.currentPage,
                 pct, ts);
      }
    } else {
      snprintf(buf, bufSize, "/screenshots/screenshot-%lu.png", ts);
    }
  }
}

void ScreenshotUtil::takeScreenshot(GfxRenderer& renderer) {
  const uint8_t* fb = renderer.getFrameBuffer();
  if (!fb) {
    LOG_ERR("SCR", "Framebuffer not available");
    return;
  }

  ScreenshotInfo info = activityManager.getScreenshotInfo();
  char filename[256];
  buildFilename(info, filename, sizeof(filename));

  bool saved = saveFramebufferAsPng(filename, fb, renderer.getDisplayWidth(), renderer.getDisplayHeight());
  if (saved) {
    LOG_DBG("SCR", "Screenshot saved to %s", filename);
  } else {
    LOG_ERR("SCR", "Failed to save screenshot");
    return;
  }

  // Invert only the border so feedback never depends on allocating a second
  // framebuffer. Applying the same operation again restores the exact pixels.
  int marginTop, marginRight, marginBottom, marginLeft;
  renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
  constexpr int borderWidth = 2;
  const int x = marginLeft + 1;
  const int y = marginTop + 1;
  const int width = renderer.getScreenWidth() - marginLeft - marginRight - 3;
  const int height = renderer.getScreenHeight() - marginTop - marginBottom - 3;
  const auto invertBorder = [&renderer, x, y, width, height]() {
    renderer.invertRect(x, y, width, borderWidth);
    renderer.invertRect(x, y + height - borderWidth, width, borderWidth);
    renderer.invertRect(x, y + borderWidth, borderWidth, height - borderWidth * 2);
    renderer.invertRect(x + width - borderWidth, y + borderWidth, borderWidth, height - borderWidth * 2);
  };
  invertBorder();
  renderer.displayBuffer();
  delay(1000);
  invertBorder();
  renderer.displayBuffer(HalDisplay::RefreshMode::HALF_REFRESH);
}

namespace {

// Minimal PNG writer: filter Up per row, then zlib with one fixed-Huffman block
// that only uses literals and distance-1 matches (run-length). A blank e-ink
// page shrinks to a few KB with no dictionary or heap. The compressed stream
// goes out as small IDAT chunks, so no seek-back or big buffer is needed.
class PngWriter {
 public:
  explicit PngWriter(HalFile& f) : file(f) {}

  bool begin(uint32_t width, uint32_t height, uint8_t bitDepth) {
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t ihdr[13];
    be32(ihdr, width);
    be32(ihdr + 4, height);
    ihdr[8] = bitDepth;
    ihdr[9] = 0;  // grayscale
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    ok = file.write(kSig, sizeof(kSig)) == sizeof(kSig);
    chunk("IHDR", ihdr, sizeof(ihdr));
    putByte(0x78);  // zlib header, fastest
    putByte(0x01);
    putBits(3, 3);  // final block, fixed Huffman
    return ok;
  }

  // One filtered row (filter byte included).
  void row(const uint8_t* data, size_t n) {
    for (size_t i = 0; i < n; i++) {
      a1 = (a1 + data[i]) % 65521;
      a2 = (a2 + a1) % 65521;
      rle(data[i]);
    }
  }

  bool finish() {
    flushRun();
    putCode(256);
    if (bitCnt > 0) putBits(0, 8 - bitCnt);
    const uint32_t adler = (a2 << 16) | a1;
    for (int s = 24; s >= 0; s -= 8) putByte(static_cast<uint8_t>(adler >> s));
    flushChunk();
    chunk("IEND", nullptr, 0);
    return ok;
  }

 private:
  HalFile& file;
  bool ok = true;
  uint8_t out[512] = {};
  size_t outLen = 0;
  uint32_t bitBuf = 0;
  int bitCnt = 0;
  uint32_t a1 = 1, a2 = 0;
  uint8_t last = 0;
  bool haveLast = false;
  int run = 0;

  static void be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
  }

  static uint32_t crc(uint32_t c, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) {
      c ^= p[i];
      for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0 - (c & 1)));
    }
    return c;
  }

  void chunk(const char* type, const uint8_t* data, size_t n) {
    uint8_t hdr[8];
    be32(hdr, n);
    memcpy(hdr + 4, type, 4);
    uint32_t c = crc(0xFFFFFFFFu, hdr + 4, 4);
    if (data) c = crc(c, data, n);
    uint8_t tail[4];
    be32(tail, ~c);
    ok = ok && file.write(hdr, 8) == 8 && (n == 0 || file.write(data, n) == n) && file.write(tail, 4) == 4;
  }

  void flushChunk() {
    if (outLen > 0) chunk("IDAT", out, outLen);
    outLen = 0;
  }

  void putByte(uint8_t b) {
    out[outLen++] = b;
    if (outLen == sizeof(out)) flushChunk();
  }

  void putBits(uint32_t v, int n) {  // LSB first
    bitBuf |= v << bitCnt;
    bitCnt += n;
    while (bitCnt >= 8) {
      putByte(static_cast<uint8_t>(bitBuf));
      bitBuf >>= 8;
      bitCnt -= 8;
    }
  }

  void putHuff(uint32_t code, int len) {  // Huffman codes go MSB first
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
    putBits(r, len);
  }

  void putCode(uint32_t v) {  // fixed literal/length alphabet
    if (v < 144)
      putHuff(0x30 + v, 8);
    else if (v < 256)
      putHuff(0x190 + v - 144, 9);
    else if (v < 280)
      putHuff(v - 256, 7);
    else
      putHuff(0xC0 + v - 280, 8);
  }

  void putMatch(int len) {  // distance 1
    static constexpr uint16_t kBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                           31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr uint8_t kExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    int i = 28;
    while (kBase[i] > len) i--;
    putCode(257 + i);
    putBits(len - kBase[i], kExtra[i]);
    putHuff(0, 5);
  }

  void flushRun() {
    if (run >= 3)
      putMatch(run);
    else
      for (int i = 0; i < run; i++) putCode(last);
    run = 0;
  }

  void rle(uint8_t b) {
    if (haveLast && b == last && run < 258) {
      run++;
      return;
    }
    flushRun();
    putCode(b);
    last = b;
    haveLast = true;
  }
};

}  // namespace

bool ScreenshotUtil::saveFramebufferAsPng(const char* filename, const uint8_t* framebuffer, int width, int height) {
  if (!framebuffer) {
    return false;
  }

  // Note: the width and height, we rotate the image 90d counter-clockwise to match the default display orientation
  int phyWidth = height;
  int phyHeight = width;

  std::string path(filename);
  size_t last_slash = path.find_last_of('/');
  if (last_slash != std::string::npos) {
    std::string dir = path.substr(0, last_slash);
    if (!Storage.exists(dir.c_str())) {
      if (!Storage.mkdir(dir.c_str())) {
        return false;
      }
    }
  }

  HalFile file;
  if (!Storage.openFileForWrite("SCR", filename, file)) {
    LOG_ERR("SCR", "Failed to save screenshot");
    return false;
  }

  // While the last gray pass is still on the panel, save its 4 levels as 2-bit
  // gray (level 0..3 = black, dark, light, white), else 1-bit (1 = white).
  const bool gray = display.grayShotReady();
  const size_t rowBytes = (phyWidth * (gray ? 2 : 1) + 7) / 8;

  // Max row = 528 px (X3) at 2 bpp = 132 bytes; fixed buffers avoid VLAs. Callers run on loopTask or the 16 KB
  // render task; the ~0.9 KB of stack here (rows + PNG chunk buffer) fits both.
  constexpr size_t kMaxRowSize = 132;
  if (rowBytes > kMaxRowSize) {
    LOG_ERR("SCR", "Row size %lu exceeds buffer capacity", static_cast<unsigned long>(rowBytes));
    file.close();
    Storage.remove(filename);
    return false;
  }

  PngWriter png(file);
  bool good = png.begin(phyWidth, phyHeight, gray ? 2 : 1);
  uint8_t cur[kMaxRowSize];
  uint8_t prev[kMaxRowSize] = {};
  uint8_t filtered[kMaxRowSize + 1];

  // rotate the image 90d counter-clockwise on-the-fly while writing to save memory
  for (int outY = 0; good && outY < phyHeight; outY++) {
    uint8_t* r = cur;
    memset(r, 0, rowBytes);
    const int srcX = width - 1 - outY;  // phyHeight == width
    for (int outX = 0; outX < phyWidth; outX++) {
      const int srcY = phyWidth - 1 - outX;  // phyWidth == height
      if (gray) {
        r[outX / 4] |= display.grayShotLevel(srcX, srcY) << (6 - 2 * (outX & 3));
        continue;
      }
      const uint8_t pixel = (framebuffer[srcY * (width / 8) + (srcX / 8)] >> (7 - (srcX % 8))) & 0x01;
      r[outX / 8] |= pixel << (7 - (outX % 8));
    }
    filtered[0] = 2;  // Up
    for (size_t i = 0; i < rowBytes; i++) filtered[i + 1] = r[i] - prev[i];
    png.row(filtered, rowBytes + 1);
    memcpy(prev, r, rowBytes);
  }
  good = good && png.finish();

  // Explicitly close() file before calling Storage.remove()
  file.close();

  if (!good) {
    LOG_ERR("SCR", "PNG write failed");
    Storage.remove(filename);
    return false;
  }

  return true;
}
