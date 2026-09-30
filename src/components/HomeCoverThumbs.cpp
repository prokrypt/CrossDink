#include "HomeCoverThumbs.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "components/themes/dashboard/DashboardTheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"
#include "components/themes/minimal/MinimalTheme.h"

namespace HomeCoverThumbs {

int coverGridThumbHeight = 0;

namespace {

using Kind = Spec::Kind;

// Width of a 2:3 thumb, as UITheme::getCoverThumbPath() and the EPUB/XTC
// generators compute it for a zero width.
uint16_t twoThirdsWidth(const uint16_t height) { return static_cast<uint16_t>((uint32_t{height} * 2 + 1) / 3); }

void add(Specs& specs, const Kind kind, const int width, const int height) {
  if (height <= 0 || specs.count >= specs.items.size()) return;
  specs.items[specs.count++] = Spec{kind, static_cast<uint16_t>(width), static_cast<uint16_t>(height)};
}

}  // namespace

Specs forActiveTheme(const std::string& bookPath, const int coverHeight) {
  Specs specs;
  const bool isEpub = FsHelpers::hasEpubExtension(bookPath);
  if (!isEpub && !FsHelpers::hasXtcExtension(bookPath)) return specs;
  if (UITheme::hasCoverGridHome()) {
    // Same file as the grid's FromSource thumb, from the loaded metadata.
    add(specs, Kind::Exact, 0, coverGridThumbHeight);
    return specs;
  }
  const Kind fitted = isEpub ? Kind::Adaptive : Kind::Exact;
  switch (static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme)) {
    case CrossPointSettings::UI_THEME::LYRA_CAROUSEL:
      add(specs, Kind::Exact, LyraCarouselTheme::kCenterThumbW, LyraCarouselTheme::kCenterThumbH);
      add(specs, Kind::Exact, LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
      break;
    case CrossPointSettings::UI_THEME::DASHBOARD:
      add(specs, fitted, DashboardMetrics::homeCoverImageWidth, DashboardMetrics::homeCoverImageHeight);
      break;
    case CrossPointSettings::UI_THEME::MINIMAL:
      add(specs, fitted, MinimalMetrics::homeCoverImageWidth, MinimalMetrics::homeCoverImageHeight);
      break;
    default:
      add(specs, Kind::Exact, 0, coverHeight);
      break;
  }
  return specs;
}

Specs forCoverGrid(const std::string& bookPath, const int thumbWidth, const int thumbHeight) {
  Specs specs;
  if (FsHelpers::hasEpubExtension(bookPath) || FsHelpers::hasXtcExtension(bookPath)) {
    add(specs, Kind::FromSource, thumbWidth, thumbHeight);
  }
  return specs;
}

std::string path(const std::string& bookPath, const std::string& coverBmpPath, const Spec& spec) {
  if (spec.kind == Kind::Adaptive) {
    return Epub(bookPath, "/.crosspoint").getAdaptiveThumbBmpPath(spec.width, spec.height);
  }
  if (spec.width == 0) return UITheme::getCoverThumbPath(coverBmpPath, spec.height);
  // Grid thumbs are exact slot sizes: no fallback to a legacy height-only thumb.
  return UITheme::getCoverThumbPath(coverBmpPath, spec.width, spec.height, spec.kind != Kind::FromSource);
}

bool generate(Epub& epub, const Spec& spec) {
  switch (spec.kind) {
    case Kind::Exact:
      return epub.generateThumbBmp(spec.width, spec.height);
    case Kind::Adaptive:
      return epub.generateAdaptiveThumbBmp(spec.width, spec.height);
    case Kind::FromSource:
      return epub.generateThumbBmpFromSource(spec.width, spec.height);
  }
  return false;
}

bool generate(const Xtc& xtc, const Spec& spec) {
  const uint16_t width = spec.width != 0 ? spec.width : twoThirdsWidth(spec.height);
  return xtc.generateThumbBmp(width, spec.height);
}

}  // namespace HomeCoverThumbs
