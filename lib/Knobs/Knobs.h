#pragma once

#include <cstddef>
#include <cstdint>

// Goodies > Knobs: tunable internal constants, listed in Knobs.def. Goodies
// builds (CROSSDINK_GOODIES) keep them in RAM, settable from the Knobs page and
// CMD:KNOB, with non-defaults in /.crosspoint/knobs.json (src/Knobs.cpp).
// Other builds see a constexpr KNOBS, so every read is the default.
struct Knobs {
#define X(group, id, type, def, min, max, step, unit) type id = def;
#include "Knobs.def"
#undef X
};

// Every default on its own min/max/step grid.
#define X(group, id, type, def, min, max, step, unit)                                                                 \
  static_assert((min) <= (def) && (def) <= (max) && (step) > 0 && ((def) - (min)) / (step) * (step) == (def) - (min), \
                #id);
#include "Knobs.def"
#undef X

#if CROSSDINK_GOODIES
extern Knobs KNOBS;
// Replaces `constexpr T NAME = value;` (T = the knob's type): a live reference
// here, the same constexpr in other builds. Namespace or class scope.
#define KNOB_ALIAS(name, id) inline const decltype(Knobs::id)& name = KNOBS.id

namespace knobs {
struct Info {
  const char* group;
  const char* id;
  int32_t def, min, max, step;
  const char* unit;
};
extern const Info INFO[];
constexpr int COUNT = 0
#define X(...) +1
#include "Knobs.def"
#undef X
    ;
int32_t get(int index);
// Clamps and snaps to the step, pushes SDK knobs, then saves unless told not to.
// Returns the value kept. Main task.
int32_t set(int index, int32_t value, bool save = true);
int find(const char* id);  // -1 when unknown
void resetAll();           // defaults, knobs.json deleted
void load(bool skipFile);  // setup(), after SD; defaults if Back is held or after 3 crash boots under 30 s
void loop();               // main loop: clears the boot counter after 30 s up
}  // namespace knobs
#else
inline constexpr Knobs KNOBS{};
#define KNOB_ALIAS(name, id) inline constexpr decltype(Knobs::id) name = KNOBS.id
#endif
