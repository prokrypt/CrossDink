// Goodies SD module ABI (x4-pro-debug, CROSSDINK_GOODIES). A module is a wasm32
// file in /debug/goodies/ that the firmware runs in the WAMR interpreter
// (src/activities/goodies/GoodyModuleActivity.cpp). It can only call the host
// functions below: there is no display-refresh, LUT, PLL, VCOM or register call,
// so the panel is driven exactly as for any other page.
//
// Exports the module provides:
//   void gd_render(void);                 draw the page (the host clears it, draws the
//                                         header with the module's name, then refreshes)
//   int  gd_input(int code, int x, int y); a key or tap; return 1 to redraw
// Back (key or header arrow) always closes the module and never reaches it.
#pragma once

#define GD_IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

enum { GD_UP = 1, GD_DOWN, GD_LEFT, GD_RIGHT, GD_CONFIRM, GD_TAP };
enum { GD_PIN_GPIO, GD_PIN_LEVEL, GD_PIN_CHANGES, GD_PIN_WAKES, GD_PIN_CHATTER };

// Screen: coordinates are relative to the page area below the header. Only
// inside gd_render(); elsewhere they do nothing. Out-of-area drawing is clipped.
GD_IMPORT(gd_width) int gd_width(void);
GD_IMPORT(gd_height) int gd_height(void);
GD_IMPORT(gd_line_height) int gd_line_height(void);
GD_IMPORT(gd_text) void gd_text(int x, int y, const char* text, int bold);
GD_IMPORT(gd_rect) void gd_rect(int x, int y, int w, int h, int fill);

// Log: one LOG_INF line tagged [GDY].
GD_IMPORT(gd_log) void gd_log(const char* text);

// Pin monitor readings (src/platform/PinMon.h).
GD_IMPORT(gd_pin_count) int gd_pin_count(void);
GD_IMPORT(gd_pin_stat) int gd_pin_stat(int pin, int field);
GD_IMPORT(gd_pinmon_enabled) int gd_pinmon_enabled(void);
GD_IMPORT(gd_pinmon_set) void gd_pinmon_set(int on);

#define GD_EXPORT(name) __attribute__((export_name(#name)))
