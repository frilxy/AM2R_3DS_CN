#pragma once

// AM2R's pause screen (rm_subscreen), on the top screen.
//
// The game draws and runs its own pause menu (map, inventory, logs, options) on the top screen, operated with its
// own buttons (Menu1/Menu2 and the D-pad). This module draws nothing and injects no input for it: it tracks whether
// the game is in its pause room, and it owns what this port puts on the bottom screen.

#include <stdbool.h>

#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif
typedef struct Renderer Renderer;

// What the bottom screen is showing. The map follows play; the other two are pages the player opens with the buttons
// along the bottom of the screen.
typedef enum {
    N3DS_BOTTOM_PAGE_MAP = 0,
    N3DS_BOTTOM_PAGE_ITEMS,
    N3DS_BOTTOM_PAGE_COUNT,
} N3DSBottomPage;

void N3DSPause_init(Runner* runner, Renderer* renderer);
// Once a frame, before the game's step: follows the pause state. Returns whether the game is paused.
bool N3DSPause_update(void);
bool N3DSPause_isPaused(void);
// In a game (global.ingame) and not on the pause screen: when the bottom screen is live.
bool N3DSPause_inPlay(void);
// Once a frame while the bottom screen is live: the buttons along its bottom edge switch pages, and the page under
// the finger gets the rest of the gestures (dragging the map, picking an item).
void N3DSPause_updateMapTouch(void);
// Which page the bottom screen is showing.
N3DSBottomPage N3DSPause_bottomPage(void);
// Switches pages, as tapping a bottom button does.
void N3DSPause_setBottomPage(N3DSBottomPage page);
