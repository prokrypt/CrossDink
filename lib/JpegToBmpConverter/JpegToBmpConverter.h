#pragma once

#include <HalStorage.h>

#include <atomic>

class Print;
class ZipFile;

class JpegToBmpConverter {
  static bool jpegFileToBmpStreamInternal(FsFile& jpegFile, Print& bmpOut, int targetWidth, int targetHeight,
                                          bool oneBit, bool crop = true, bool adaptiveContain = false,
                                          bool imageLevels = false);

 public:
  // Polled once per output row; while the flag is set, a running conversion fails fast.
  // shortcut: one process-wide flag, fine while the Home cover worker is the only canceller; pass it per call if
  // another task ever needs to cancel its own decodes.
  static void setCancelFlag(const std::atomic<bool>* flag);
  static bool jpegFileToBmpStream(FsFile& jpegFile, Print& bmpOut, bool crop = true, bool imageLevels = false);
  // Convert with custom target size (for thumbnails)
  // Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
  static bool jpegFileTo1BitBmpStreamWithSize(FsFile& jpegFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                              bool adaptiveContain = false);
};
