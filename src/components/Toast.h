#pragma once

#include "components/themes/BaseTheme.h"

class GfxRenderer;

// The one look, place and dwell for every transient confirmation: a black box
// with a white edge, centered on screen, kept up for DURATION_MS.
namespace Toast {
constexpr unsigned long DURATION_MS = 1000UL;

Rect bounds(const GfxRenderer& renderer, const char* msg);
// Into the framebuffer only; the caller's next refresh shows it.
void draw(const GfxRenderer& renderer, const char* msg);
// draw() plus a refresh. Callers that keep it up delay(DURATION_MS) after.
void show(const GfxRenderer& renderer, const char* msg);
}  // namespace Toast
