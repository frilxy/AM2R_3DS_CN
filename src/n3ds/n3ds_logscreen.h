#pragma once

// AM2R's log screens on the bottom screen: the list (categories down the left, each category's entries under it, the
// selected entry's picture on the right) and the entry itself (its picture on the left, its text on the right).
//
// The game draws its own versions on rm_subscreen. Those objects only exist in that room, so this rebuilds the same
// two pages for the bottom screen from the game's own state: global.log[] says which entries have been found, the
// [Logs] / [NewLogs] sections hold their titles and text, and bgLogImgNN holds their pictures.

#include "../renderer.h"
#include "../runner.h"

#include <stdbool.h>

void N3DSLogScreen_init(Runner* runner, Renderer* renderer);
// The game has the log state and strings this needs.
bool N3DSLogScreen_available(void);
// Draws the list on the bottom screen (call after the game's frame, during play).
void N3DSLogScreen_draw(void);
// Touch: a tap on an entry opens it, a tap elsewhere in the list scrolls or changes category. Returns whether the
// tap was taken.
void N3DSLogScreen_tap(int32_t x, int32_t y);
// Touch: dragging the list scrolls it.
void N3DSLogScreen_drag(int32_t dx, int32_t dy);
// Number of entries the game says have been found, for the port's own bookkeeping.
int32_t N3DSLogScreen_foundCount(void);
