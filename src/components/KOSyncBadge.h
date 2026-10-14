#pragma once

#include "activities/home/RecentBookProgress.h"
#include "components/themes/BaseTheme.h"

namespace freeink {
struct Icon;
}

// The KOSync badge (cloud / cloud-off / cloud-download) next to a book's % on
// Home. Home sets the state of the book a theme is about to draw; themes call
// drawAt() where their % label sits, so no theme signature carries it.
namespace KOSyncBadge {
constexpr int SIZE = 20;
constexpr int GAP = 6;  // between the badge and the % text
// Slots: a theme drawing one book uses slot 0; Lyra Extended uses one per book.
constexpr int HOME_SLOTS = 3;

// Home only, around a theme's book draw; None (the default) draws nothing.
void setHomeState(RecentBookProgress::SyncBadge state, int slot = 0);
// True when slot has a badge to draw.
bool hasHomeState(int slot = 0);
// The badge's box at x, centred on the text line at lineY. The box is recorded
// for takeRect() even when there is nothing to draw, so a later badge can go there.
void drawAt(const GfxRenderer& renderer, int x, int lineY, int lineHeight, bool black = true, int slot = 0);
// The box drawAt() last used for slot since the previous call (width 0: none);
// clears every slot's box and state.
Rect takeRect(int slot = 0);
// Clears box to white and draws state's icon in it (the toast-time repaint).
void repaint(const GfxRenderer& renderer, const Rect& box, RecentBookProgress::SyncBadge state);
// The icon for a state; null for None.
const freeink::Icon* icon(RecentBookProgress::SyncBadge state);
}  // namespace KOSyncBadge
