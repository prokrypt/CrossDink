#pragma once

// Goodies > Display test line scripts (debug builds, CROSSDINK_GOODIES).
// One op per line, '#' starts a comment. Format: docs/goodies.md.

#include <cstdint>
#include <string>
#include <vector>

namespace display_script {

enum class OpCode : uint8_t {
  Name,      // text
  Fill,      // a0: 1 = black
  Checker,   // a0: cell px
  HStripes,  // a0: stripe px
  VStripes,  // a0: stripe px
  Box,       // a0..a3: x y w h (logical), a4: 1 = white
  Text,      // sample text over the whole screen
  Invert,    //
  Refresh,   // a0: Mode
  Frames,    // a0: DU frames (1..63)
  Pll,       // a0: PLL byte during DU refreshes, 0 = default
  Scrub,     // a0: 0 = Half, 1 = DU (before the next Fast/DU refresh)
  Wait,      // a0: ms
  Repeat,    // a0: count, a1: index of the matching End
  End,       // a0: index of the matching Repeat
  Note,      // text
  Ask,       // text = question; options = the two answers
  Label,     // text drawn in a white band at the top
  DrawText,  // a0 a1: x y; text
  Pick,      // a0..a5: grid x y cellW cellH cols rows; text = question; options = one name per cell
  Confirm,   // as Ask; the first answer stops the test
  Swing,     // a0: frames; balanced DU swing of every pixel to the framebuffer (UC8179 Half-as-scrub)
  Null,      // a0: frames per phase; sources at GND, VCOM at VCOM_DC (UC8179 null discharge)
};

enum class Mode : uint8_t { Full, Half, Fast, Du };

struct Op {
  OpCode code;
  int32_t a[6] = {};
  std::string text;
  std::vector<std::string> options;
};

struct Script {
  std::string name;
  std::vector<Op> ops;
  int errorLine = 0;  // 1-based; 0 = parsed
  const char* error = nullptr;
};

struct BuiltIn {
  const char* name;
  const char* source;
};

// Built-in tests, stored in flash.
extern const BuiltIn BUILT_INS[];
extern const int BUILT_IN_COUNT;

// Parses `len` bytes of `src`. On failure `errorLine`/`error` say where.
Script parse(const char* src, size_t len);

const char* modeName(Mode mode);

}  // namespace display_script
