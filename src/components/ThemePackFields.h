#pragma once

#include <ArduinoJson.h>

#include <cstdint>

#include "components/themes/BaseTheme.h"

// The ThemeMetrics fields an SD theme pack may set, by JSON key, with the range a
// pack value is clamped to. Fields left out: the status-bar lane and reader status
// bar (they move the reader's text area), Home cover sizes and counts (thumbnail
// cache keys and array bounds), and button-hint sizes (compile-time in theme code).
namespace ThemePackFields {

enum class Kind : uint8_t { Int, Bool, Ratio, Enum8 };  // Ratio: float, stored in thousandths

struct Field {
  const char* key;
  uint16_t offset;
  Kind kind;
  int16_t min;
  int16_t max;
};

enum class Parse : uint8_t { Bad, Ok, Clamped };

int count();
const Field& at(int index);
int find(const char* key);  // -1 when unknown
// Stored value for a pack's JSON value: clamped number, or 0/1 for a bool field.
Parse parse(int index, JsonVariantConst value, int32_t& out);
void apply(ThemeMetrics& metrics, int index, int32_t value);
int32_t read(const ThemeMetrics& metrics, int index);  // stored form, for tests

}  // namespace ThemePackFields
