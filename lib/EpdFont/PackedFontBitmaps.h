#pragma once

#include <Memory.h>
#include <PackedAsset.h>

#include "EpdFont.h"

// Built-in fonts generated with `fontconvert.py --pack-bitmaps` carry
// EpdFontData::bitmap == nullptr plus one raw-DEFLATE bitmap blob. This
// inflates the blob (PSRAM first, then the default heap: the UI cannot draw
// text without it) and points `font` at a copy of its EpdFontData stored in
// the same buffer, whose bitmap points at the inflated bytes. `storage` owns
// that buffer for as long as `font` is in use; calling again with a filled
// `storage` does nothing. On failure `font` is left unchanged and its glyphs
// draw blank.
bool attachPackedBitmaps(EpdFont& font, const PackedAsset& bitmaps, HeapByteBuffer& storage);
