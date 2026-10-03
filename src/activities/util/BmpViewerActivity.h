#pragma once

#include <Memory.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  void onFrontlightPanelClosed() override;

 private:
  void requestImageRedraw();
  void drawImage();
  void loadSiblingImages();
  bool renderPngImage();
  bool showImage(bool gray, const std::function<bool()>& drawImageFrame);

  // A picture decoded once into its Direct gray planes (2-bit levels as two physical bit planes,
  // no button hints). Their AND is the B/W image.
  struct DecodedImage {
    std::string path;
    uint8_t look = 0;                         // DirectPixelWriter::bwImages it was decoded for
    bool gray = false;                        // shows with a gray refresh; false = one FAST refresh
    int x = 0, y = 0, width = 0, height = 0;  // picture rect, for night-mode polarity
    HeapByteBuffer lsb;
    HeapByteBuffer msb;
  };
  const DecodedImage* decodedImage(uint8_t look);
  bool decodeBmpLevels(DecodedImage& image);
  bool decodePngLevels(DecodedImage& image);
  bool showDecoded(const DecodedImage& image);
  void drawHints() const;
  void doSetSleepCover();
  void showContextMenu();
  void promptDeleteImage();
  void pinSleepFavorite();
  void unpinSleepFavorite();
  void pinBootFavorite();
  void unpinBootFavorite();

  std::string filePath;
  std::vector<std::string> siblingImages;
  int currentImageIndex = -1;
  // Set when the image must be decoded and drawn again: on entry, after changing
  // images, and after an overlay (top panel, menus, prompts) drew over it. Other
  // update requests (battery, USB) leave the image on screen as is.
  std::atomic<bool> needsImageRedraw{true};
  // Set by loop() on input that leaves or replaces the image: drawImage() stops
  // between decode passes before anything reaches the panel. Cleared when a new
  // draw is requested.
  std::atomic<bool> drawCancelled{false};
  // The last few decoded pictures (PSRAM, 96 KB each), newest last; going back to one skips the
  // decode. Freed in onExit().
  std::vector<DecodedImage> recentImages;
};
