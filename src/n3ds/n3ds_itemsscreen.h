#pragma once

// AM2R's items panel on the bottom screen: the four boxes the game's pause screen shows (suits, beams, misc, boots),
// Samus in the middle, the energy and missile counts along the top, and the selected item's description below.
//
// The game draws its own version on rm_subscreen; this draws the same information in 320x240 from the game's own
// state (global.item[], playerhealth, missiles, the [ItemsMenu] strings), so it can sit on the bottom screen while
// play carries on above it.

#include "../renderer.h"
#include "../runner.h"

#include <stdbool.h>

void N3DSItemsScreen_init(Runner* runner, Renderer* renderer);
// The game has the item state and strings this needs.
bool N3DSItemsScreen_available(void);
// Draws the panel on the bottom screen (call after the game's frame, during play).
void N3DSItemsScreen_draw(void);
// Touch: a tap selects the item under the point (and shows its description); returns whether one was hit.
void N3DSItemsScreen_tap(int32_t x, int32_t y);
// The items the game's state says are owned, for the port's own bookkeeping.
int32_t N3DSItemsScreen_ownedCount(void);
