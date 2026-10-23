#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "AppCapabilities.h"
#if CROSSDINK_SCALABLE_FONTS
#include <HalScalableFont.h>
#include <SdCardFontRegistry.h>
#endif

class GfxRenderer;

// The persisted filename-font choice fills missing characters in shared UI fonts.
class FilenameFontSystem {
 public:
  static constexpr const char* FONT_DIR = "/.crossdink/languages/fonts";
  bool discover(std::vector<std::string>& names);
  bool ensureLoaded(GfxRenderer& renderer);
  void release(GfxRenderer& renderer);
  // File-transfer screens can replace fonts or hand the SD card to the host.
  // Keep the suspension across child screens until storage is safe again.
  void suspend(GfxRenderer& renderer);
  void resume();
  void invalidate() { dirty_.store(true, std::memory_order_release); }
  void invalidateForPath(const char* path);
  uint32_t fingerprint() const;

 private:
  std::atomic<bool> dirty_{true};
#if CROSSDINK_SCALABLE_FONTS
  unsigned suspensionCount_ = 0;
  char attemptedFamily_[64] = {};
  std::unique_ptr<HalScalableFont> regular_, bold_;
  std::string regularPath_, boldPath_;
  bool scanFamily(const char* name, SdCardFontFamilyInfo& family);
#endif
};

extern FilenameFontSystem filenameFontSystem;
