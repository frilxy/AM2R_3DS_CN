#pragma once

// AM2R's map on the bottom screen during play, drawn from the game's own map data (global.map / global.dmap) the
// way its pause screen draws it, centred on Samus's cell.

#include "../renderer.h"
#include "../runner.h"

#include <stdbool.h>

void N3DSLiveMap_init(Runner* runner, Renderer* renderer);
// The game has the map sprites and map arrays this needs.
bool N3DSLiveMap_available(void);
// Once a frame, before the draw: advances the frame counter the marker's pulse is timed from.
void N3DSLiveMap_beginFrame(void);
// Draws the map on the bottom screen (call after the game's frame, during play).
void N3DSLiveMap_draw(void);
// The frame every bottom-screen page shares, so a page's content sits between the same two bars whichever page it is:
// a title bar flush with the top, the content, a footer bar, and the buttons along the bottom. Anything a page draws
// outside the content band is covered, because each page draws its bars after its content.
#define N3DS_BOTTOM_WIDTH 320
#define N3DS_BOTTOM_HEIGHT 240
#define N3DS_BOTTOM_BAR_H 16                  // the title bar, and the footer bar
#define N3DS_BOTTOM_CONTENT_TOP N3DS_BOTTOM_BAR_H
#define N3DS_BOTTOM_CONTENT_BOTTOM 180        // the footer starts here
#define N3DS_BOTTOM_FOOTER_H 20
#define N3DS_BOTTOM_BUTTON_H 40
#define N3DS_BOTTOM_BUTTON_TOP (N3DS_BOTTOM_HEIGHT - N3DS_BOTTOM_BUTTON_H)
#define N3DS_BUTTON_COUNT 2                   // equipment, map

// The green bar the game's own sub-screen pages put across their top, shared by every page here: the game's
// bg_SubScrBottom stretched across the width. Drawn at `y`; the caller puts its title text on top.
void N3DSLiveMap_drawBar(float y, float height);
// The three-button strip along the bottom edge, shared by every bottom-screen page. `current` is the slot whose page
// is showing (0 items, 1 map, 2 log).
void N3DSLiveMap_drawButtons(int32_t current);
// Touch: drag moves the map away from Samus, a tap turns a screen point into the cell under it.
void N3DSLiveMap_panBy(float dx, float dy);
void N3DSLiveMap_resetPan(void);
void N3DSLiveMap_screenToCell(int32_t sx, int32_t sy, int32_t* outX, int32_t* outY);
