#include "BookLoadingFeedback.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include "components/UITheme.h"

bool BookLoadingFeedback::showIfDue(GfxRenderer& renderer) {
  if (!renderer.hasFrameBuffer() || !isDue(millis())) return false;
  finish();
  shown = true;
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  return true;
}

void BookLoadingFeedback::showIndexing(GfxRenderer& renderer) {
  const bool replaceLoading = shown;
  finish();
  if (!renderer.hasFrameBuffer()) return;
  if (replaceLoading) renderer.clearScreen();
  GUI.drawPopup(renderer, tr(STR_INDEXING));
}
