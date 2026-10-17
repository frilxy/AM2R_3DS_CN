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
