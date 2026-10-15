#include "ThemePackFields.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>

namespace ThemePackFields {
namespace {

template <typename T>
constexpr Kind kindOf();
template <>
constexpr Kind kindOf<int>() {
  return Kind::Int;
}
template <>
constexpr Kind kindOf<bool>() {
  return Kind::Bool;
}
template <>
constexpr Kind kindOf<float>() {
  return Kind::Ratio;
}
template <>
constexpr Kind kindOf<ThemeTabBarAppearance>() {
  return Kind::Enum8;
}

#define TP_FIELD(name, lo, hi) {#name, offsetof(ThemeMetrics, name), kindOf<decltype(ThemeMetrics::name)>(), lo, hi}
#define TP_BOOL(name) TP_FIELD(name, 0, 1)

// Ranges hold every built-in theme's value (ThemePackFieldsTest). Touch-sized rows
// (menu, tab bar, keyboard keys) keep minimums near the 44 px touch target.
const Field kFields[] = {
    TP_FIELD(batteryBarHeight, 20, 80),
    TP_FIELD(headerHeight, 30, 140),
    TP_FIELD(verticalSpacing, 0, 40),
    TP_FIELD(previewPadding, 0, 40),
    TP_FIELD(previewHeightPercent, 10, 60),
    TP_FIELD(contentSidePadding, 0, 60),
    TP_FIELD(listRowHeight, 24, 80),
    TP_FIELD(listWithSubtitleRowHeight, 40, 120),
    TP_FIELD(listRowGap, 0, 20),
    TP_FIELD(listRowRadius, 0, 30),
    TP_FIELD(listInset, 0, 40),
    TP_FIELD(listSidePadding, 0, 40),
    TP_FIELD(listSelectionStyle, 0, 5),  // freeink::ui::SelectionStyle
    TP_FIELD(listScrollWidth, 1, 10),
    TP_FIELD(listScrollSide, 0, 1),
    TP_BOOL(listTitleBold),
    TP_FIELD(headerSidePadding, 0, 40),
    TP_FIELD(headerUnderlineSize, 0, 6),
    TP_FIELD(headerTitleAlign, 0, 2),  // freeink::ui::TextAlign
    TP_FIELD(headerBatterySide, 0, 1),
    TP_BOOL(headerBatteryDetached),
    TP_FIELD(menuRowHeight, 42, 100),
    TP_FIELD(menuSpacing, 0, 30),
    TP_FIELD(tabSpacing, 0, 30),
    TP_FIELD(tabBarHeight, 40, 80),
    TP_FIELD(tabBarAppearance, 0, 1),
    TP_FIELD(scrollBarWidth, 1, 10),
    TP_FIELD(scrollBarRightOffset, 0, 20),
    TP_FIELD(homeTopPadding, 20, 120),
    TP_FIELD(homeMenuTopOffset, 0, 60),
    TP_FIELD(progressBarHeight, 4, 30),
    TP_FIELD(keyboardKeyHeight, 44, 80),
    TP_FIELD(keyboardKeySpacing, 0, 16),
    TP_BOOL(keyboardCenteredText),
    TP_FIELD(keyboardVerticalOffset, -30, 30),
    TP_FIELD(keyboardTextFieldWidthPercent, 50, 100),
    TP_FIELD(keyboardWidthPercent, 70, 100),
    TP_FIELD(popupTopOffsetRatio, 0, 400),
    TP_FIELD(popupMarginX, 0, 40),
    TP_FIELD(popupMarginY, 0, 40),
    TP_FIELD(popupFrameThickness, 0, 6),
    TP_FIELD(popupCornerRadius, 0, 30),
    TP_BOOL(popupTextBold),
    TP_BOOL(popupTextInverted),
    TP_FIELD(popupTextBaselineOffsetY, -10, 10),
    TP_FIELD(popupProgressBarHeight, 1, 12),
    TP_BOOL(popupProgressDrawOutline),
    TP_BOOL(popupProgressClampPercent),
    TP_BOOL(popupProgressFillInverted),
    TP_BOOL(popupProgressOutlineInverted),
    TP_FIELD(optionPopupItemSpacing, 0, 24),
    TP_FIELD(optionPopupInnerPadding, 0, 40),
    TP_FIELD(optionPopupSelectionHPadding, 0, 40),
    TP_FIELD(optionPopupSelectionVPadding, 0, 24),
    TP_FIELD(optionPopupTitleGap, 0, 32),
    TP_BOOL(optionPopupUseSmallFont),
    TP_BOOL(optionPopupOptionFontBold),
    TP_FIELD(optionPopupSelectionRadius, 0, 40),
    TP_BOOL(optionPopupSelectionLight),
    TP_BOOL(optionPopupDrawAllRows),
    TP_FIELD(optionPopupDialogSideMargin, 0, 60),
    TP_BOOL(optionPopupTitleSeparator),
    TP_FIELD(textFieldHorizontalPadding, 0, 20),
    TP_FIELD(textFieldNormalThickness, 1, 4),
    TP_FIELD(textFieldCursorThickness, 1, 6),
    TP_FIELD(textFieldLineEndOffset, -10, 10),
};

#undef TP_BOOL
#undef TP_FIELD

}  // namespace

int count() { return static_cast<int>(std::size(kFields)); }

const Field& at(const int index) { return kFields[index]; }

int find(const char* key) {
  const auto it = std::find_if(std::begin(kFields), std::end(kFields),
                               [key](const Field& f) { return std::strcmp(f.key, key) == 0; });
  return it == std::end(kFields) ? -1 : static_cast<int>(it - std::begin(kFields));
}

Parse parse(const int index, const JsonVariantConst value, int32_t& out) {
  const Field& f = kFields[index];
  if (f.kind == Kind::Bool) {
    if (!value.is<bool>()) return Parse::Bad;
    out = value.as<bool>() ? 1 : 0;
    return Parse::Ok;
  }
  if (!value.is<float>()) return Parse::Bad;
  float x = value.as<float>();
  if (std::isnan(x)) return Parse::Bad;
  if (f.kind == Kind::Ratio) x *= 1000.0f;
  // Clamp before rounding so huge numbers cannot overflow the conversion.
  const auto n = static_cast<int32_t>(std::lround(std::clamp(x, -32768.0f, 32767.0f)));
  out = std::clamp<int32_t>(n, f.min, f.max);
  return out == n ? Parse::Ok : Parse::Clamped;
}

void apply(ThemeMetrics& metrics, const int index, const int32_t value) {
  const Field& f = kFields[index];
  auto* p = reinterpret_cast<uint8_t*>(&metrics) + f.offset;
  switch (f.kind) {
    case Kind::Int: {
      const int v = value;
      std::memcpy(p, &v, sizeof(v));
      break;
    }
    case Kind::Bool: {
      const bool v = value != 0;
      std::memcpy(p, &v, sizeof(v));
      break;
    }
    case Kind::Ratio: {
      const float v = static_cast<float>(value) / 1000.0f;
      std::memcpy(p, &v, sizeof(v));
      break;
    }
    case Kind::Enum8: {
      const auto v = static_cast<uint8_t>(value);
      std::memcpy(p, &v, sizeof(v));
      break;
    }
  }
}

int32_t read(const ThemeMetrics& metrics, const int index) {
  const Field& f = kFields[index];
  const auto* p = reinterpret_cast<const uint8_t*>(&metrics) + f.offset;
  switch (f.kind) {
    case Kind::Int: {
      int v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
    case Kind::Bool: {
      bool v;
      std::memcpy(&v, p, sizeof(v));
      return v ? 1 : 0;
    }
    case Kind::Ratio: {
      float v;
      std::memcpy(&v, p, sizeof(v));
      return static_cast<int32_t>(std::lround(v * 1000.0f));
    }
    case Kind::Enum8: {
      uint8_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    }
  }
  return 0;
}

}  // namespace ThemePackFields
