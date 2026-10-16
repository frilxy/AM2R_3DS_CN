#pragma once

// AM2R on the 3DS: changes to the running game made from the front end (no change to the game's files).
//  - Nintendo buttons: the Xbox prompt icons and names become the 3DS's (A/B and X/Y swapped by position, L/R/ZL/ZR,
//    Select), "XBox 360 Joypad" becomes "Nintendo 3DS".
//  - Options: the Display page (window and screen settings that don't apply here) becomes a Cheats page; the
//    Keyboard settings row is hidden.
//  - Cheats: master switch, unlimited health, unlimited ammo, super beam / bombs / missiles. Kept in
//    sdmc:/3ds/am2r/cheats.ini.
//  - The water/lava ripple filter (oWaterFXV2: a copy of the screen redrawn in 1-pixel strips every frame) is off:
//    it costs most of a frame on the 3DS.

#include "../runner.h"

#include <stdbool.h>

// After the VM is ready (installs the script hooks). Does nothing for a game without AM2R's scripts.
void N3DSAm2r_init(Runner* runner);
// Once a frame, after the game's step.
void N3DSAm2r_update(void);
// The width the game lays its picture out for: 320 + oControl.widescreen_space while the Community Updates' own
// widescreen is on (426), 0 otherwise. The game centres that picture in the window it is told about, so the window
// has to be this wide for it to land on screen.
int32_t N3DSAm2r_gameWidth(void);
