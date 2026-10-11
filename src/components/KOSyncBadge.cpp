#include "KOSyncBadge.h"

#include <GfxRenderer.h>

#include "components/UiAppHelpers.h"
#include "components/icons/syncIcons.h"

namespace {
using RecentBookProgress::SyncBadge;
SyncBadge homeState = SyncBadge::None;  // render task only, like the theme draws that read it
Rect lastBox;
}  // namespace

namespace KOSyncBadge {

void setHomeState(const SyncBadge state) { homeState = state; }

const freeink::Icon* icon(const SyncBadge state) {
  switch (state) {
    case SyncBadge::Synced:
      return &icon_cloud_20;
    case SyncBadge::Unsynced:
      return &icon_cloud_off_20;
    case SyncBadge::ServerAhead:
      return &icon_cloud_download_20;
    case SyncBadge::None:
      break;
  }
  return nullptr;
}

void drawAt(const GfxRenderer& renderer, const int x, const int lineY, const int lineHeight, const bool black) {
  lastBox = Rect(x, lineY + (lineHeight - SIZE) / 2, SIZE, SIZE);
  const freeink::Icon* badge = icon(homeState);
  if (!badge) return;
  // Opaque backing: the badge may overlap other pixels on tight layouts.
  renderer.fillRect(lastBox.x, lastBox.y, SIZE, SIZE, !black);
  drawLucideIcon(renderer, *badge, lastBox.x, lastBox.y, black);
}

Rect takeRect() {
  const Rect box = lastBox;
  lastBox = Rect();
  return box;
}

void repaint(const GfxRenderer& renderer, const Rect& box, const SyncBadge state) {
  renderer.fillRect(box.x, box.y, box.width, box.height, false);
  if (const freeink::Icon* badge = icon(state)) drawLucideIcon(renderer, *badge, box.x, box.y);
}

}  // namespace KOSyncBadge
