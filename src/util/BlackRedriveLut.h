#pragma once

#ifndef SIMULATOR
#include <FreeInkDisplay.h>
#endif

#include <cstdint>

// Fast refreshes on the UC8179 panel (X4 Pro) use the OTP waveform, which never
// drives pixels that stay black, so a header or label that sits still through
// many fast repaints (transfer progress, Wi-Fi bars) slowly fades to grey.
// While this scope is alive, fast refreshes use the keyboard's DU register LUT
// instead: 6 frames that also re-drive unchanged blacks (the key-fade fix), and
// shorter than the OTP fast refresh. Full/Half refreshes are unaffected, and
// other panels ignore it.
//
// Wrap only the displayBuffer() of a screen that repaints in a loop; the
// driver latches the choice when the refresh starts, so screens pushed on top
// keep the stock waveform (DU on menus leaves ghosting).
class BlackRedriveLut {
 public:
  static constexpr uint8_t FRAMES = 6;
  // A long single-phase drive for scrubs that must erase dense old text
  // without the Half flash (6 frames leaves it faintly visible).
  static constexpr uint8_t SCRUB_FRAMES = 15;

  // scrub: make the next fast refresh in this scope a DU scrub with the long
  // drive: every pixel is driven through its complement once, erasing the
  // previous screen's ghost without the Half flash. UC8179 only.
  explicit BlackRedriveLut(const bool enable = true, const bool scrub = false) : enabled(enable) {
#ifndef SIMULATOR
    if (!enabled) return;
    freeink::Uc8179KbdExperiment exp;
    exp.flags = freeink::Uc8179KbdExperiment::KbdLut;
    exp.lutFrames = scrub ? SCRUB_FRAMES : FRAMES;
    freeink::setUc8179KbdExperiment(&exp);
    if (scrub) freeink::requestUc8179DuScrubNext();
#else
    (void)scrub;
#endif
  }
  ~BlackRedriveLut() {
#ifndef SIMULATOR
    if (enabled) freeink::setUc8179KbdExperiment(nullptr);
#endif
  }
  BlackRedriveLut(const BlackRedriveLut&) = delete;
  BlackRedriveLut& operator=(const BlackRedriveLut&) = delete;

 private:
  bool enabled;
};
