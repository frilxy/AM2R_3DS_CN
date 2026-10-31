#pragma once

// AM2R scripts and events rewritten in C ("Fast scripts" on the Display page): the ones that cost the most per frame in
// the interpreter. Each replaces the game's code only when that code's bytecode is exactly the version it was written
// from (AM2R 1.5.5, the Community Updates); anything else keeps running the game's own code. A native gives the game's
// code back (calls it) whenever it meets something it doesn't expect (a variable missing or of another type).

#include "../runner.h"

#include <stdbool.h>

// After N3DSAm2r_init. Installs the natives when enabled.
void N3DSAm2rNative_init(Runner* runner);
// Turns the natives on or off (takes effect from the next call).
void N3DSAm2rNative_setEnabled(bool enabled);
bool N3DSAm2rNative_enabled(void);
// The dark rooms' light effect (the light map): off skips building it, so those rooms are drawn fully lit.
void N3DSAm2rNative_setLighting(bool on);
bool N3DSAm2rNative_lighting(void);
// How many of the natives match this game's code (0: none apply, the option does nothing).
int32_t N3DSAm2rNative_available(void);
