#pragma once
#include <GfxRenderer.h>
struct TestTheme {
  int draws = 0;
  void drawPopup(GfxRenderer&, const char*) { ++draws; }
};
extern TestTheme GUI;
