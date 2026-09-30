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

  // firstFrame: the frame that enters the screen (it lands on the previous
  // screen) keeps the stock OTP Fast. The DU LUT drives each pixel one way only,
  // so a DU entry frame (or a DU scrub) adds charge instead of clearing it.
  explicit BlackRedriveLut(const bool enable = true, const bool firstFrame = false)
      : enabled(enable && !firstFrame) {
#ifndef SIMULATOR
    if (!enabled) return;
    freeink::Uc8179KbdExperiment exp;
    exp.flags = freeink::Uc8179KbdExperiment::KbdLut;
    exp.lutFrames = FRAMES;
    freeink::setUc8179KbdExperiment(&exp);
#endif
  }
  // The next Half refresh runs as a SCRUB_FRAMES DU scrub instead of the
  // flashing GC Half (reader ghost cleanups, keyboard close). UC8179 only;
  // other panels keep the Half.
  static void scrubNextHalf() {
#ifndef SIMULATOR
    freeink::requestUc8179HalfAsDuScrubNext(SCRUB_FRAMES);
#endif
  }
  // True on the frame a screen enters its redrive state: that frame lands on
  // the previous screen, so it stays OTP Fast. `wasOn` is render-task state.
  static bool entering(bool& wasOn, const bool on) {
    const bool first = on && !wasOn;
    wasOn = on;
    return first;
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
