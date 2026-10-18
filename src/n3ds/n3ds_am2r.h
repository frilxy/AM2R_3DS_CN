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

// The game's own HUD switch (the global opshowhud, whose Options row this port's Display page carries instead). The
// port keeps the player's choice and pushes it onto the game every frame, because the game's own option loader would
// otherwise put its saved value back. Until the player sets it here the game's own setting is left alone.
bool N3DSAm2r_hudEnabled(void);
void N3DSAm2r_setHudEnabled(bool on);

// How far the picture is cut off the right of the screen (Wide: 13; 1x and the game-sized-window modes: 0). The game's
// HUD rides the picture's transform, so its right-hand block (map, Metroid counter, laid out past widescreen_space)
// loses those columns too; the native HUD takes this much off that block's x offsets, which is what sets the HUD's
// right margin.
double N3DSAm2r_pictureSpace(void);
double N3DSAm2r_hudRightInset(void);

// How far the picture is cut off the left of the screen (Wide: 13; 1x and the game-sized-window modes: 0). The game's
// HUD rides the picture's transform, so its left-hand block (health, tanks, ammo) loses those columns too; the native
// HUD moves that block back in by this much. Each side uses its own crop, so both blocks keep the same margin.
double N3DSAm2r_hudLeftInset(void);
