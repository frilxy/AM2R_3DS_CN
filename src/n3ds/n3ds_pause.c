#include "n3ds_pause.h"

#include "n3ds_gml.h"

#include "../log.h"
#include "../runner.h"

#include <string.h>

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
} gPause;

void N3DSPause_init(Runner* runner, Renderer* renderer) {
    (void) renderer; // nothing is drawn here: the game draws and operates its own pause screen
    memset(&gPause, 0, sizeof(gPause));
    gPause.runner = runner;
    N3DSGml_init(runner);
    gPause.roomSubscreen = N3DSGml_roomIndex("rm_subscreen");
    gPause.roomTransition = N3DSGml_roomIndex("rm_transition");
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
