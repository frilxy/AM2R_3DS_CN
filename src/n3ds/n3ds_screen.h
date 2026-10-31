#pragma once

#include <stdbool.h>

// The top screen's mode (N3DS_SCREEN_MODE in cmake/n3ds.cmake is the default; a game front end can change it).
typedef enum {
    N3DS_SCREEN_WIDE,      // upstream's widescreen hack: views grow to the 5:3 screen, 1:1 pixels
    N3DS_SCREEN_STRETCH,   // the game's size stretched to the whole screen
    N3DS_SCREEN_PILLARBOX, // the screen's size as the game's window, the game 1:1 and centred
    N3DS_SCREEN_1X,        // the game's size, 1:1 and centred (the GUI too)
    N3DS_SCREEN_2X,        // the game's size at 2x, centred: the middle of the picture, the rest off screen
    N3DS_SCREEN_MODE_COUNT
} N3DSScreenMode;

N3DSScreenMode N3DS_getScreenMode(void);
void N3DS_setScreenMode(N3DSScreenMode mode);

// Frameskip: two game steps per drawn frame (30 fps for a 60-step room), so the game keeps its speed.
bool N3DS_getFrameskip(void);
void N3DS_setFrameskip(bool on);

// A small frame counter in the top screen's bottom-left corner.
bool N3DS_getFpsCounter(void);
void N3DS_setFpsCounter(bool on);
