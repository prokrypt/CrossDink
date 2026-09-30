#pragma once

#include <array>
#include <cstdint>
#include <string>

class Epub;
class Xtc;

// The cover thumbnails Home draws for a recent book under the active theme.
// Home and the readers share this so a reader can make them before Home needs
// them. Generation passes no renderer, so it is safe on a worker task.
namespace HomeCoverThumbs {

struct Spec {
  enum class Kind : uint8_t {
    Exact,       // cropped to width x height; width 0 means 2:3 of height
    Adaptive,    // EPUB only: fitted inside width x height
    FromSource,  // cover grid: parsed from the OPF, no metadata cache load
  };
  Kind kind = Kind::Exact;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct Specs {
  std::array<Spec, 2> items{};
  uint8_t count = 0;
};

// Thumb height of the cover grid's first slot (the book being read), set by
// Home once laid out; 0 until Home has been shown since boot.
extern int coverGridThumbHeight;

// Thumbs of the active theme for the book being read. coverHeight is the
// theme's homeCoverHeight. Books that are neither EPUB nor XTC get none.
Specs forActiveTheme(const std::string& bookPath, int coverHeight);
// The cover grid sizes each slot from its layout.
Specs forCoverGrid(const std::string& bookPath, int thumbWidth, int thumbHeight);

// Where the spec's thumb lives; empty when it has no path.
std::string path(const std::string& bookPath, const std::string& coverBmpPath, const Spec& spec);
bool generate(Epub& epub, const Spec& spec);
bool generate(Xtc& xtc, const Spec& spec);

}  // namespace HomeCoverThumbs
