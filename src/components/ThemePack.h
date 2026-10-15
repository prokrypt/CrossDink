#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "components/themes/BaseTheme.h"
#include "util/BootReason.h"

namespace freeink {
struct Icon;
}

// SD-card theme packs: /.themes/<name>/theme.json or /themes/<name>/theme.json,
// hidden root first (the /.fonts and /fonts rule). A pack names a built-in base
// theme and overrides its metrics (ThemePackFields) and icons (1-bit BMP, 24 or
// 32 px). Data only: nothing in a pack reaches refresh modes, waveforms or panel
// registers. Read once when picked or at boot; drawing never touches the card.
namespace ThemePack {

// Brings the loaded pack in line with SETTINGS.uiThemePack and, when one is
// active, sets SETTINGS.uiTheme to its base. The card is read only when the name
// changed since the last attempt, or with force (the picker's re-pick). Callers
// hold RenderLock once the render task runs. A pack that fails to load logs why,
// queues a toast and leaves the built-in theme in place; its name stays saved.
void sync(bool force);
bool active();
// The pack's icon for this slot and size (24 or 32), or nullptr for the built-in one.
const freeink::Icon* icon(UIIcon icon, uint32_t size);
// Pack folder names in both roots, sorted (directory listing only).
std::vector<std::string> list();
// Boot, before the first sync: the second crash reboot in a row with a pack set
// turns the pack off (RTC record, per crossdink/plan/rtc-memory-compat.md).
void bootGuard(esp_reset_reason_t reason);
// A toast queued by sync() or bootGuard(), once; nullptr when none.
const char* takeToast();

}  // namespace ThemePack
