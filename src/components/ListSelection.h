#pragma once

// CROSSDINK_APP_CAP_TOUCH comes from the build flags (validated in AppCapabilities.h).

// Touch devices open lists with no row selected. A list shows its selection
// once a nav button is pressed on that screen (ActivityManager eats the first
// press to reveal it), or while a popup opened by tapping a row is up. Home
// and other non-list screens keep their own preselection.
namespace ListSelection {
// Set by the render task from the activity it is about to render.
inline bool revealed = false;
// A popup opened by a row tap is up (or about to be) over that row.
inline bool tapRowShown = false;
// Last rendered frame drew a list with its selection hidden (render task
// writes, main loop reads).
inline bool hidOnScreen = false;
inline bool hidThisFrame = false;

// True when a list should draw its selected row. Records a hidden list for
// the button reveal.
inline bool shown() {
#if CROSSDINK_APP_CAP_TOUCH
  if (revealed || tapRowShown) return true;
  hidThisFrame = true;
  return false;
#else
  return true;
#endif
}
}  // namespace ListSelection
