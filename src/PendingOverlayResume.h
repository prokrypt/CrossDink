#pragma once

#include <cstdint>
#include <string>
#include <utility>

enum class PendingOverlayOrigin : uint8_t { None = 0, Reader, Home, FileBrowser, Library };
enum class PendingOverlayType : uint8_t { None = 0, ReaderDrawer, FrontlightDrawer };

struct PendingOverlayResume {
  PendingOverlayOrigin origin = PendingOverlayOrigin::None;
  PendingOverlayType overlay = PendingOverlayType::None;
  // Library returns use tab for sort method and pane for descending order.
  uint8_t tab = 0;
  uint8_t pane = 0;
  int16_t selectedIndex = 0;
  int16_t scrollPosition = 0;
  std::string bookPath;  // File Browser returns reopen the folder with this book selected
  std::string libraryQuery;
  bool returnHomeAfterReaderFlow = false;
  // A book can override global orientation. Preserve that layout while a
  // reader-originated sync restarts through the network boot flow.
  uint8_t readerOrientation = 0;
  bool preserveReaderOrientation = false;

  bool valid() const {
    if (origin == PendingOverlayOrigin::Library) return true;
    if (origin == PendingOverlayOrigin::FileBrowser) return !bookPath.empty();
    return origin != PendingOverlayOrigin::None && overlay != PendingOverlayType::None;
  }
  bool returnsToBookList() const {
    return valid() && (origin == PendingOverlayOrigin::FileBrowser || origin == PendingOverlayOrigin::Library);
  }
  void clear() { *this = PendingOverlayResume{}; }
};

inline bool consumePendingOverlayResumeOnce(PendingOverlayResume& stored, PendingOverlayResume& value) {
  if (!stored.valid()) return false;
  value = std::move(stored);
  stored.clear();
  return true;
}
