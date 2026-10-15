#include "KOSyncBadge.h"

#include <GfxRenderer.h>

#include "components/UiAppHelpers.h"
#include "components/icons/syncIcons.h"

namespace {
using RecentBookProgress::SyncBadge;
// Render task only, like the theme draws that read them.
SyncBadge homeState[KOSyncBadge::HOME_SLOTS] = {};
Rect lastBox[KOSyncBadge::HOME_SLOTS];
bool validSlot(const int slot) { return slot >= 0 && slot < KOSyncBadge::HOME_SLOTS; }
}  // namespace

namespace KOSyncBadge {

void setHomeState(const SyncBadge state, const int slot) {
  if (validSlot(slot)) homeState[slot] = state;
}

bool hasHomeState(const int slot) { return validSlot(slot) && homeState[slot] != SyncBadge::None; }

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

void drawAt(const GfxRenderer& renderer, const int x, const int lineY, const int lineHeight, const bool black,
            const int slot) {
  if (!validSlot(slot)) return;
  const Rect box(x, lineY + (lineHeight - SIZE) / 2, SIZE, SIZE);
  lastBox[slot] = box;
  const freeink::Icon* badge = icon(homeState[slot]);
  if (!badge) return;
  // Opaque backing: the badge may overlap other pixels on tight layouts.
  renderer.fillRect(box.x, box.y, SIZE, SIZE, !black);
  drawLucideIcon(renderer, *badge, box.x, box.y, black);
}

Rect takeRect(const int slot) {
  const Rect box = validSlot(slot) ? lastBox[slot] : Rect();
  for (int i = 0; i < HOME_SLOTS; ++i) {
    lastBox[i] = Rect();
    homeState[i] = SyncBadge::None;
  }
  return box;
}

void repaint(const GfxRenderer& renderer, const Rect& box, const SyncBadge state) {
  renderer.fillRect(box.x, box.y, box.width, box.height, false);
  if (const freeink::Icon* badge = icon(state)) drawLucideIcon(renderer, *badge, box.x, box.y);
}

}  // namespace KOSyncBadge
