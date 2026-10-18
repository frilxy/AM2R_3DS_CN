#pragma once

// The localisation credits, shown on the bottom screen while the game's own credits roll plays on the top screen.
//
// The names come from the language file's [Header] section - Author and Author2 - which the game itself never reads:
// it only takes "Language" as the display name, so those fields would otherwise never appear anywhere. The credits
// room is rm_credits; the bottom screen is drawn by this port, so this is the one place they can go without touching
// the game's own credit roll, which is assembled from English literals inside data.win.

#include "../runner.h"
#include "../renderer.h"

void N3DSCredits_init(Runner* runner, Renderer* renderer);
// Draws the credits on the bottom screen if the game is in rm_credits; does nothing anywhere else.
void N3DSCredits_draw(void);
