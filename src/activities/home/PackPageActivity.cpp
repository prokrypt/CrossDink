#include "PackPageActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <cstdio>

#include "MappedInputManager.h"
#include "components/ThemePack.h"
#include "components/Toast.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"

namespace TPH = ThemePackHome;

namespace {
void showToast(GfxRenderer& renderer, const char* text) {
  RenderLock lock;
  Toast::show(renderer, text);
}
}  // namespace

std::unique_ptr<PackPageActivity> PackPageActivity::open(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                         const char* id, const PackHomeUi::Available& available,
                                                         const uint8_t depth) {
  std::unique_ptr<PackPageActivity> page;
  if (depth > kMaxDepth) {
    LOG_ERR("THM", "page %s: pages nest more than %d deep", id, kMaxDepth);
  } else {
    // Scratch for the parse only; the page keeps its own copy.
    auto layout = makeUniqueNoThrow<TPH::Layout>();
    if (!layout) LOG_ERR("THM", "page %s: out of memory", id);
    if (layout && ThemePack::page(id, *layout)) {
      page = makeUniqueNoThrow<PackPageActivity>(renderer, mappedInput, *layout, available, depth);
      if (!page) LOG_ERR("THM", "page %s: out of memory", id);
    }
  }
  if (!page) showToast(renderer, tr(STR_THEME_PACK_ERROR));
  return page;
}

PackPageActivity::PackPageActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                   const ThemePackHome::Layout& layout, const PackHomeUi::Available& available,
                                   const uint8_t depth)
    : Activity("PackPage", renderer, mappedInput),
      ui(renderer, layout, static_cast<int16_t>(TouchHeaderBackButton::contentTop(renderer, mappedInput))),
      available(available),
      depth(depth) {
  std::snprintf(title, sizeof(title), "%s", layout.texts[0]);
}

void PackPageActivity::onEnter() {
  Activity::onEnter();
  ui.begin(noBooks, available);
  requestUpdate();
}

void PackPageActivity::activate() {
  const PackHomeUi::Target picked = ui.targetAt(selected);
  if (!picked.entry) return;
  const TPH::Entry& entry = *picked.entry;
  if (entry.action == TPH::Action::Page) {
    auto child = open(renderer, mappedInput, entry.path, available, static_cast<uint8_t>(depth + 1));
    if (!child) {
      requestUpdate();
      return;
    }
    // A pick on the child goes on to Home; Back there returns here.
    startActivityForResult(std::move(child), [this](const ActivityResult& result) {
      if (const auto* pick = std::get_if<ThemePageResult>(&result.data)) {
        setResult(ThemePageResult{*pick});
        finish();
      }
    });
    return;
  }
  // The pack names a card path: it may be gone or renamed. Say so here, not after leaving.
  if ((entry.action == TPH::Action::Folder || entry.action == TPH::Action::Book) && !Storage.exists(entry.path)) {
    LOG_ERR("THM", "Theme pack entry not found: %s", entry.path);
    showToast(renderer, tr(STR_NOT_FOUND));
    requestUpdate();
    return;
  }
  setResult(ThemePageResult{static_cast<uint8_t>(entry.action), entry.path});
  finish();
}

void PackPageActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    finish();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finishAfterBackPress();
    return;
  }
  const int touched = ui.tappedStop(mappedInput);
  if (ui.app.invalidated()) requestUpdate();
  if (touched >= 0) {
    selected = touched;
    activate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activate();
    return;
  }
  const int count = ui.stopCount();
  if (count > 0) {
    buttonNavigator.onNext([this, count] {
      selected = ButtonNavigator::nextIndex(selected, count);
      requestUpdate();
    });
    buttonNavigator.onPrevious([this, count] {
      selected = ButtonNavigator::previousIndex(selected, count);
      requestUpdate();
    });
  }
}

void PackPageActivity::render(RenderLock&&) {
  renderer.clearScreen();
  {
    FilenameUiFontScope fonts(ui.target, renderer);  // per-glyph fallbacks for the pack's own title
    TouchHeaderBackButton::draw(renderer, ui.target, TouchHeaderBackButton::headerRect(renderer, mappedInput), title,
                                false);
  }
  ui.select(selected);
  ui.render();
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
