#pragma once

#include <HalStorage.h>

#include <atomic>

class Print;

class PngToBmpConverter {
  static bool pngFileToBmpStreamInternal(FsFile& pngFile, Print& bmpOut, int targetWidth, int targetHeight, bool oneBit,
                                         bool crop = true, bool adaptiveContain = false, bool imageLevels = false);

 public:
  // Polled once per output row; while the flag is set, a running conversion fails fast.
  // shortcut: one process-wide flag, fine while the Home cover worker is the only canceller; pass it per call if
  // another task ever needs to cancel its own decodes.
  static void setCancelFlag(const std::atomic<bool>* flag);
  static bool pngFileToBmpStream(FsFile& pngFile, Print& bmpOut, bool crop = true, bool imageLevels = false);
  static bool pngFileTo1BitBmpStreamWithSize(FsFile& pngFile, Print& bmpOut, int targetMaxWidth, int targetMaxHeight,
                                             bool adaptiveContain = false);
};
