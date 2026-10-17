#include "n3ds_pause.h"

#include "n3ds_gml.h"
#include "n3ds_itemsscreen.h"
#include "n3ds_livemap.h"
#include "n3ds_input.h"

#include "../log.h"
#include "../runner.h"

#include <string.h>

// The live map's geometry, which the bottom-screen drawing and the touch handling have to agree on: a cell is 8x8 and
// Samus's cell sits in the middle of the 320x240 screen. (Kept in step with n3ds_livemap.c's N3DS_LIVEMAP_W/H/CELL.)
#define N3DS_MAP_CELL 8
#define N3DS_MAP_SCREEN_W 320
#define N3DS_MAP_SCREEN_H 240

// The button strip along the bottom edge, which the map module draws. Kept here as well so a tap can be told to have
// landed on it before the page underneath sees it.
#define N3DS_BOTTOM_WIDTH 320
#define N3DS_BOTTOM_BUTTON_COUNT 3
#define N3DS_BOTTOM_BUTTON_H 40
#define N3DS_BOTTOM_BUTTON_TOP (240 - N3DS_BOTTOM_BUTTON_H)

// Whether the game is in its own pause screen.
//
// The pause menu is AM2R's rm_subscreen, running in the game's VM: its script steps draw it on the top screen and
// its own Menu1/Menu2/D-pad bindings operate it, exactly as they do on any other room. The front end therefore has
// no drawing or input to do for it - it only has to know that the screen changed, so that main.c can pin the window
// height (rm_subscreen lays itself out for a tall 320x568 surface, which would letterbox the picture away on the
// 400x240 top screen). A room_change to rm_subscreen goes through rm_transition first, which this follows.

static struct {
    Runner* runner;
    bool paused;
    bool inGame; // the game's own global.ingame: a save is loaded, so there is play (and a map) to show
    int32_t roomSubscreen, roomTransition;
    // Touch on the bottom-screen map: dragging pans it, a tap puts the marker down (or lifts it again). A drag has to
    // be told from a tap, or every pan would drop a marker where the finger went down.
    bool touchDown;
    bool dragging;
    int32_t touchStartX, touchStartY, touchLastX, touchLastY;
    N3DSBottomPage page;    // what the bottom screen shows: the live map, or a page opened with the buttons
} gPause;

void N3DSPause_init(Runner* runner, Renderer* renderer) {
    (void) renderer; // nothing is drawn here: the game draws and operates its own pause screen
    memset(&gPause, 0, sizeof(gPause));
    gPause.runner = runner;
    N3DSGml_init(runner);
    gPause.roomSubscreen = N3DSGml_roomIndex("rm_subscreen");
    gPause.roomTransition = N3DSGml_roomIndex("rm_transition");
    gPause.page = N3DS_BOTTOM_PAGE_MAP;
    if (gPause.roomSubscreen < 0) logInfo("Pause: no rm_subscreen in this game, pause tracking off\n");
}

bool N3DSPause_isPaused(void) {
    return gPause.paused;
}

// In a game and not paused: the state the bottom screen's map belongs to. global.ingame is the game's own flag - it
// goes up when a save is loaded and down when the player leaves the game - so the map appears and disappears with the
// game itself, and the bottom screen stays black everywhere else (title, file select, options, the pause screen).
bool N3DSPause_inPlay(void) {
    return gPause.inGame && !gPause.paused;
}

// ===[ Touch on the bottom-screen map ]===
// The map is drawn by this port, not by the game, so the pan is kept here too. The game has no map camera during play
// (oMapCamera only exists on its own pause map page), which is why driving that object could never work: there is no
// instance to drive. A drag moves the port's own offset and the drawing follows it.

// A move shorter than this is a tap: without it a slightly wobbly finger would pan the map and still drop a marker.
#define N3DS_MAP_TOUCH_SLOP 6

// A tap on the map puts the marker on that cell, or lifts it if it is already there.
static void N3DSPause_tapMap(int32_t sx, int32_t sy) {
    int32_t cx = 0, cy = 0;
    N3DSLiveMap_screenToCell(sx, sy, &cx, &cy);
    // The map is 74 x 57 cells inside its border; the game's own pause screen clamps to the same range.
    if (cx < 3) cx = 3;
    if (cx > 76) cx = 76;
    if (cy < 3) cy = 3;
    if (cy > 59) cy = 59;

    double hasMarker = 0.0, mx = -1.0, my = -1.0;
    N3DSGml_getGlobal("mapmarker", -1, &hasMarker);
    N3DSGml_getGlobal("mapmarkerx", -1, &mx);
    N3DSGml_getGlobal("mapmarkery", -1, &my);
    if (hasMarker != 0.0 && (int32_t) mx == cx && (int32_t) my == cy) {
        N3DSGml_setGlobal("mapmarker", 0.0);
        logInfo("Live map: marker lifted at %d,%d\n", (int) cx, (int) cy);
        return;
    }
    N3DSGml_setGlobal("mapmarker", 1.0);
    N3DSGml_setGlobal("mapmarkerx", (double) cx);
    N3DSGml_setGlobal("mapmarkery", (double) cy);
    logInfo("Live map: marker set at %d,%d\n", (int) cx, (int) cy);
}

N3DSBottomPage N3DSPause_bottomPage(void) {
    return gPause.page;
}

void N3DSPause_setBottomPage(N3DSBottomPage page) {
    if (page >= N3DS_BOTTOM_PAGE_COUNT || page == gPause.page) return;
    gPause.page = page;
    logInfo("Bottom screen: page %d\n", (int) page);
}

// A tap in the button strip along the bottom edge switches page.
static bool N3DSPause_tapButton(int32_t x, int32_t y) {
    if (y < N3DS_BOTTOM_BUTTON_TOP) return false;
    int32_t slot = x * N3DS_BOTTOM_BUTTON_COUNT / N3DS_BOTTOM_WIDTH;
    if (slot < 0) slot = 0;
    if (slot >= N3DS_BOTTOM_BUTTON_COUNT) slot = N3DS_BOTTOM_BUTTON_COUNT - 1;
    // Left to right: items, map, log.
    N3DSPause_setBottomPage(slot == 0 ? N3DS_BOTTOM_PAGE_ITEMS
        : (slot == 1 ? N3DS_BOTTOM_PAGE_MAP : N3DS_BOTTOM_PAGE_LOGS));
    return true;
}

// Called once a frame while the bottom screen is live: the buttons along the bottom edge switch pages, and the page
// under the finger gets the rest.
void N3DSPause_updateMapTouch(void) {
    int32_t x, y;
    bool held = N3DSInput_touch(&x, &y);
    if (held && !gPause.touchDown) {
        gPause.touchDown = true;
        gPause.dragging = false;
        gPause.touchStartX = gPause.touchLastX = x;
        gPause.touchStartY = gPause.touchLastY = y;
        return;
    }
    if (held) {
        if (!gPause.dragging) {
            int32_t adx = x - gPause.touchStartX, ady = y - gPause.touchStartY;
            if (adx < 0) adx = -adx;
            if (ady < 0) ady = -ady;
            if (adx > N3DS_MAP_TOUCH_SLOP || ady > N3DS_MAP_TOUCH_SLOP) gPause.dragging = true;
        }
        // Only the map pans; the other pages are picked from, not dragged.
        if (gPause.dragging && gPause.page == N3DS_BOTTOM_PAGE_MAP) {
            N3DSLiveMap_panBy((float) (x - gPause.touchLastX), (float) (y - gPause.touchLastY));
        }
        gPause.touchLastX = x;
        gPause.touchLastY = y;
        return;
    }
    if (gPause.touchDown) {
        gPause.touchDown = false;
        if (!gPause.dragging) {
            if (!N3DSPause_tapButton(gPause.touchStartX, gPause.touchStartY)) {
                if (gPause.page == N3DS_BOTTOM_PAGE_MAP) N3DSPause_tapMap(gPause.touchStartX, gPause.touchStartY);
                else if (gPause.page == N3DS_BOTTOM_PAGE_ITEMS) {
                    N3DSItemsScreen_tap(gPause.touchStartX, gPause.touchStartY);
                }
            }
        }
        gPause.dragging = false;
    }
}

static bool N3DSPause_inPauseRoom(void) {
    Runner* runner = gPause.runner;
    if (gPause.roomSubscreen < 0 || runner->currentRoom == NULL) return false;
    int32_t room = runner->currentRoomIndex;
    if (room == gPause.roomSubscreen) return true;
    if (room != gPause.roomTransition) return false;
    // The transition into the pause screen (room_change(rm_subscreen, ...)).
    double target = -1.0;
    return N3DSGml_getGlobal("targetroom", -1, &target) && (int32_t) target == gPause.roomSubscreen;
}

bool N3DSPause_update(void) {
    bool paused = N3DSPause_inPauseRoom();
    if (paused != gPause.paused) {
        logInfo(paused ? "Pause: the game's pause screen is up\n" : "Pause: back to the game\n");
    }
    gPause.paused = paused;
    // global.ingame drives the bottom screen: set while a save is being played, cleared when the player leaves the
    // game, so it is re-read every frame rather than latched.
    double inGame = 0.0;
    bool playing = gPause.roomSubscreen >= 0 && N3DSGml_getGlobal("ingame", -1, &inGame) && inGame != 0.0;
    if (playing != gPause.inGame) {
        logInfo(playing ? "Bottom screen: in a game, the map is on\n" : "Bottom screen: not in a game, black\n");
    }
    gPause.inGame = playing;
    return paused;
}
