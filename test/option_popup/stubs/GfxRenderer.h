#pragma once

#include <string>
#include <vector>

#include "EpdFontFamily.h"

class GfxRenderer {
 public:
  int getScreenWidth() const { return 800; }
  int getScreenHeight() const { return 480; }
  int getLineHeight(int) const { return 20; }
  int getTextWidth(int, const char* text, EpdFontFamily::Style = EpdFontFamily::REGULAR) const {
    return static_cast<int>(std::string(text).size()) * 8;
  }
  int getSpaceWidth(int, EpdFontFamily::Style = EpdFontFamily::REGULAR) const { return 8; }
  std::vector<std::string> wrappedText(int, const char* text, int, int maxLines, EpdFontFamily::Style) const {
    if (maxLines <= 0 || text == nullptr || text[0] == '\0') return {};
    return {text};
  }
  void setBeforeDisplay(void (*fn)(const void*), const void* ctx) {
    beforeDisplay = fn;
    beforeDisplayCtx = ctx;
  }
  void displayBuffer() const {
    if (beforeDisplay) {
      const auto fn = beforeDisplay;
      beforeDisplay = nullptr;
      fn(beforeDisplayCtx);
    }
    ++displays;
  }
  int displayCount() const { return displays; }
  void drawLine(int, int, int, int) const { ++drawnLines; }
  void fillPolygon(const int*, const int*, int, bool) const { ++drawnTriangles; }
  int lineCount() const { return drawnLines; }
  int triangleCount() const { return drawnTriangles; }

 private:
  mutable void (*beforeDisplay)(const void*) = nullptr;
  mutable const void* beforeDisplayCtx = nullptr;
  mutable int displays = 0;
  mutable int drawnLines = 0;
  mutable int drawnTriangles = 0;
};
