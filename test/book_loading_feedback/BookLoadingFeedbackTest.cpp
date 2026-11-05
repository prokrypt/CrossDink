#include <cassert>
#include <cstdint>

#include "BookLoadingFeedback.h"
#include "components/UITheme.h"

uint32_t testNow = 0;
TestTheme GUI;

int main() {
  GfxRenderer renderer;
  BookLoadingFeedback feedback;
  testNow = 10000;
  assert(!feedback.showIfDue(renderer));  // Direct reader construction is quiet.

  feedback.start(10000);
  testNow = 10299;
  assert(!feedback.showIfDue(renderer));
  testNow = 10300;
  renderer.available = false;
  assert(!feedback.showIfDue(renderer));  // Never touch lent build scratch.
  renderer.available = true;
  assert(feedback.showIfDue(renderer));
  assert(GUI.draws == 1);
  testNow = 20000;
  assert(!feedback.showIfDue(renderer));  // One refresh per open.

  feedback.start(testNow);
  feedback.finish();  // Fast first page, failure, or an existing Indexing popup.
  testNow += 1000;
  assert(!feedback.showIfDue(renderer));

  feedback.start(UINT32_MAX - 100);
  BookLoadingFeedback handedOff = feedback;
  testNow = 198;
  assert(!handedOff.showIfDue(renderer));
  testNow = 199;
  assert(handedOff.showIfDue(renderer));  // Timer survives handoff and clock wrap.
  assert(GUI.draws == 2);
  BookLoadingFeedback afterPopup = handedOff;
  assert(!afterPopup.showIfDue(renderer));

  // Indexing cancels a pending Loading; it clears only over a visible Loading notice.
  feedback.start(testNow);
  feedback.showIndexing(renderer);
  assert(GUI.draws == 3 && renderer.clears == 0);
  assert(!afterPopup.showIfDue(renderer));
  testNow += 1000;
  assert(!feedback.showIfDue(renderer));
  feedback.start(testNow - BookLoadingFeedback::DELAY_MS);
  assert(feedback.showIfDue(renderer));
  feedback.showIndexing(renderer);
  assert(GUI.draws == 5 && renderer.clears == 1);
  feedback.showIndexing(renderer);  // Indexing over Indexing keeps the old behavior.
  assert(GUI.draws == 6 && renderer.clears == 1);
  renderer.available = false;
  feedback.showIndexing(renderer);  // Never touch lent build scratch.
  assert(GUI.draws == 6);
}
