#pragma once

// The game's own sub-screen pages, drawn by the game onto an offscreen surface and shown on the bottom screen.
//
// AM2R already has the items panel and the log screens: oSubScreenSuit/Beam/Misc/Boots, oLogScreen, oLogEntry and
// their friends, all of them objects that live in rm_subscreen. Rather than rebuild those pages from measurements,
// this runs their own Draw events with a surface bound as the target, then shows that surface on the bottom screen.
// The layout, the fonts and the Chinese wrapping are the game's own, so they cannot drift from it.
//
// The pieces were each confirmed: the VM lets an instance be set as `self` (oControl, object 427, is always alive),
// and calling draw_gui_map that way returns cleanly. Surfaces are created and bound through the renderer, the same
// way the game's own surface_create / surface_set_target builtins do.

#include "../renderer.h"
#include "../runner.h"

#include <stdbool.h>

// Which of the game's pages to draw.
typedef enum {
    N3DS_GAMEPAGE_ITEMS = 0,
    N3DS_GAMEPAGE_MAP,
    N3DS_GAMEPAGE_LOGS,
    N3DS_GAMEPAGE_COUNT,
} N3DSGamePage;

void N3DSGamePage_init(Runner* runner, Renderer* renderer);
// The game has the objects this draws.
bool N3DSGamePage_available(void);
// Draws a page through the game's own code onto a surface, then shows that surface on the bottom screen. Returns
// false when it could not, so the caller can fall back to a page this port draws itself.
bool N3DSGamePage_draw(N3DSGamePage page);
