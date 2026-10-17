#pragma once

// AM2R's pause screen (rm_subscreen), on the top screen.
//
// The game draws and runs its own pause menu (map, inventory, logs, options) on the top screen, operated with its
// own buttons (Menu1/Menu2 and the D-pad). This module draws nothing and injects no input: it only tracks whether
// the game is in its pause room, so the rest of the front end (main.c's window height) can follow.

#include <stdbool.h>

#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif
typedef struct Renderer Renderer;

void N3DSPause_init(Runner* runner, Renderer* renderer);
// Once a frame, before the game's step: follows the pause state. Returns whether the game is paused.
bool N3DSPause_update(void);
bool N3DSPause_isPaused(void);
// In a game (global.ingame) and not on the pause screen: when the bottom screen shows the map.
bool N3DSPause_inPlay(void);
// Once a frame while the map is on the bottom screen: dragging pans it, a tap puts the marker down or lifts it.
void N3DSPause_updateMapTouch(void);
