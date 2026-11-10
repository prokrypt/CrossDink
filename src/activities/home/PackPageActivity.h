#pragma once

#include <memory>
#include <vector>

#include "activities/Activity.h"
#include "components/PackHomeUi.h"
#include "util/ButtonNavigator.h"

// A theme pack user page (pages/<id>.json, ThemePackHome::parsePage): a titled list
// or tile grid of Home entries under a back-button header. Picking an entry ends
// the page with a ThemePageResult and Home opens it as if picked there; a page:
// entry opens that page on top, at most kMaxDepth deep. Back always returns.
class PackPageActivity final : public Activity {
 public:
  static constexpr uint8_t kMaxDepth = 4;
  // Reads the page; nullptr (logged, toast shown) when its file is missing or bad,
  // memory is short, or pages already nest kMaxDepth deep. depth: 1 from Home.
  static std::unique_ptr<PackPageActivity> open(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* id,
                                                const PackHomeUi::Available& available, uint8_t depth);

  PackPageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const ThemePackHome::Layout& layout,
                   const PackHomeUi::Available& available, uint8_t depth);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  void activate();
  PackHomeUi ui;
  const PackHomeUi::Available available;
  const uint8_t depth;
  char title[ThemePackHome::kTextBytes];
  int selected = 0;
  ButtonNavigator buttonNavigator;
  const std::vector<RecentBook> noBooks;  // a page shows no covers
};
