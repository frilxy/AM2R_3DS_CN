#pragma once

#include <stdbool.h>

// The top screen's mode (N3DS_SCREEN_MODE in cmake/n3ds.cmake is the default; a game front end can change it).
typedef enum {
    N3DS_SCREEN_WIDE,      // widescreen: for a game with its own widescreen, the picture 1:1 filling the screen (the
                           // columns past the screen's width given up evenly); otherwise upstream's hack, where the
                           // views grow to the 5:3 screen, 1:1 pixels
    N3DS_SCREEN_STRETCH,   // the game's size stretched to the whole screen
    N3DS_SCREEN_PILLARBOX, // the screen's size as the game's window, the game 1:1 and centred
    N3DS_SCREEN_1X,        // point-for-point: the game's own 320x240 4:3 picture at 1:1, bars at the sides (the
                           // game's widescreen is off in this mode, so nothing is scaled or cropped)
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
