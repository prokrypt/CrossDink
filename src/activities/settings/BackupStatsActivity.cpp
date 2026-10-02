#include "BackupStatsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/reader/StatsBackup.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"

void BackupStatsActivity::onEnter() {
  Activity::onEnter();
  backupFileName[0] = '\0';
  // Settings already asked for confirmation; back up right away.
  runBackup();
}

void BackupStatsActivity::onExit() { Activity::onExit(); }

void BackupStatsActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const Rect header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  TouchHeaderBackButton::draw(renderer, header, tr(STR_BACKUP_NOW), false);
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int top = (pageHeight - lineHeight) / 2;
  const int detailY = top + lineHeight + metrics.verticalSpacing;

  if (state == SUCCESS) {
    renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BACKUP_STATS_DONE), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, detailY, backupFileName[0] != '\0' ? backupFileName : "-");

    const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_BACKUP_STATS_FAILED), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, detailY, tr(STR_CHECK_SERIAL_OUTPUT));

  const auto labels = mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void BackupStatsActivity::runBackup() {
  state = backupGlobalStats(true, backupFileName, sizeof(backupFileName)) ? SUCCESS : FAILED;
  requestUpdate();
}

void BackupStatsActivity::loop() {
  if (TouchHeaderBackButton::wasTapped(mappedInput, renderer)) {
    goBack();
    return;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    goBack();
  }
}
