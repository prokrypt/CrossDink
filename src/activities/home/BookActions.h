#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "FileBrowserActionActivity.h"
#include "activities/ActivityResult.h"

class GfxRenderer;
class MappedInputManager;
class Activity;

namespace BookActions {

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      bool includeRemoveFromRecents);
bool hasClearableBookCache(const std::string& path);
bool canSendNearby(const std::string& path);
void clearFileMetadata(const std::string& fullPath);
bool clearBookCache(const std::string& fullPath);
bool deleteBookStats(const std::string& fullPath);
std::unique_ptr<Activity> createReadingStatsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::string& fullPath, const std::string& title);
bool resetBookReaderSettings(const std::string& fullPath);
std::vector<std::string> epubRenderModeOptions();
uint8_t epubRenderModeDisplayIndex(uint8_t renderMode);
uint8_t epubRenderModeForDisplayIndex(uint8_t displayIndex);
std::string confirmationHeading(StrId actionLabelId);
bool isBookCompleted(const std::string& fullPath);
// Read-only variant for list rows: writes nothing, so a never-opened book
// costs only its content key and one stats read.
bool isListedBookCompleted(const std::string& fullPath);
bool isBookStatsTrackingEnabled(const std::string& fullPath);
bool toggleBookStatsTracking(const std::string& fullPath, bool& enabled);
bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed);

// What a host screen gives runSharedAction: open shows a child dialog (Library wraps it
// to stop its thumbnail task first), refresh redraws the host once the action is done.
struct Host {
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::function<void(std::unique_ptr<Activity>&&, ActivityResultHandler)> open;
  std::function<void()> refresh;
};

// Runs the book actions Library and File browser share. Returns false for any other
// action, which the host handles itself.
bool runSharedAction(FileBrowserAction action, const std::string& fullPath, const std::string& title, const Host& host);

}  // namespace BookActions
