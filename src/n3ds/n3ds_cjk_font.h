#pragma once

// The Chinese fonts, for the 3DS: `font_replace` hands the port a TTF the game's own data.win has no glyphs for, and
// this loads the pack tools/n3ds/fontgen/make_cjk_font.py baked out of those fonts instead of parsing them at boot.
//
// The pack carries one 1024x1024 texture page (1-bit rows), the glyph rectangles for each font and size the game asks
// for, and is read from next to the TTF it belongs to. Everything here is one-time work at the first font_replace,
// which is why it lives outside the renderer: the renderer only knows how to hand out a runtime texture page.

#include "../runner.h"

#include <stdbool.h>
#include <stdint.h>

// Replaces fontIndex's glyphs with the ones the pack has for ttfPath at emSize, and points the font at the pack's
// texture page. Returns false (leaving the font as it was) when the pack is missing, unreadable, or does not carry that
// font at that size -- the font then keeps whatever the game shipped, which is what draws English today.
bool N3DSCjkFont_apply(Runner* runner, int32_t fontIndex, const char* ttfPath, float emSize);
