#pragma once

// AM2R's pause screen on the bottom screen, driven by touch.
//
// While the game is in its pause screen (rm_subscreen, or the transition into it) the game's screen is routed to
// the bottom screen (320x240, AM2R's own size, 1:1) and the top screen keeps the last frame of play. Touches are
// turned into the presses the pause screen already understands (its own Menu1/Menu2 bindings and the D-pad), plus a
// few variable writes where a press can't express the gesture (dragging the map, putting the marker on a cell,
// picking an inventory row). The buttons keep working as before.

#include <stdbool.h>

#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif
typedef struct Renderer Renderer;

void N3DSPause_init(Runner* runner, Renderer* renderer);
// Once a frame, right after C3D_FrameBegin and before the game's step: follows the pause state (freezes the top
// screen, routes the game's screen) and turns this frame's touch into presses. Returns whether the game is paused.
bool N3DSPause_update(void);
bool N3DSPause_isPaused(void);
// Whether the bottom screen should show the pause screen's map page as it last was (during play).
bool N3DSPause_showMapOnBottom(void);
// In a game (global.ingame) and not on the pause screen.
bool N3DSPause_inPlay(void);
