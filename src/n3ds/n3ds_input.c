#include "n3ds_input.h"
#include "n3ds_gml.h"
#include "n3ds_pause.h"
#include "n3ds_platform_config.h"
#include "../random.h"

#include "../log.h"
#include "../runner.h"
#include "../runner_gamepad.h"
#include "../runner_keyboard.h"
#include "../utils.h"

#include <3ds.h>
#include <stb/ds/stb_ds.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

// 3DS buttons -> one virtual gamepad (slot 0, same layout as the Switch port) and the PC keyboard keys.
// Optional test harness: N3DS_SD_DIR "harness.txt", one command per line, frames counted from the first room:
//   press <frame> <BUTTON> [frames]   hold a button (A B X Y L R ZL ZR START SELECT UP DOWN LEFT RIGHT)
//   touch <frame> <x> <y> [frames] [x2 y2]   touch the bottom screen (sliding to x2,y2 over the frames)
//   screenshot <frame>                save the top screen to N3DS_SD_DIR "shots/frame_<n>.png"
//   exit <frame>                      write N3DS_SD_DIR "done.txt" and quit
//
// Start+Select together toggles the debug monitor. So that the chord never reaches the game, a Start or Select press
// reaches the game N3DS_CHORD_FRAMES frames late, so a chord can be caught before either button gets there.

typedef struct {
    u32 key;
    int32_t gml; // GameMaker virtual key
} N3DSKeyMap;

// With N3DS_KEYBOARD_MIRROR the D-pad/circle pad, Start and Select are also the keyboard's arrows, Enter and Escape.
// Off for AM2R: it treats any held keyboard binding as "playing on keyboard" (global.controltype = 0) and shows
// keyboard prompts.
#ifdef N3DS_KEYBOARD_MIRROR
static const N3DSKeyMap gKeyMap[] = {
    { KEY_DUP, VK_UP },
    { KEY_DDOWN, VK_DOWN },
    { KEY_DLEFT, VK_LEFT },
    { KEY_DRIGHT, VK_RIGHT },
    { KEY_START, VK_ENTER },
    { KEY_SELECT, VK_ESCAPE },
};
#endif

typedef struct {
    int32_t kind; // 0 press, 1 screenshot, 2 exit, 3 touch, 4 goto room, 5 place Samus
    int32_t frame;
    int32_t length;
    u32 key;
    int32_t anchor; // index into gAnchors, -1 = frames since start
    int32_t x, y, x2, y2;
    char room[48]; // goto
} N3DSHarnessCmd;

#define N3DS_CHORD_FRAMES 3

typedef struct {
    u32 history; // down/up for the last frames, newest in bit 0
} N3DSChordButton;

// "after_room <name>": the following commands count frames from the first entry into that room
// (after the previous anchor fired), so scripts survive timing differences such as intro length.
typedef struct {
    char room[48];
    int32_t frame; // -1 until entered
} N3DSHarnessAnchor;

static N3DSHarnessCmd* gHarness = NULL;
static N3DSHarnessAnchor* gAnchors = NULL;
static int32_t gFrame = 0;
// "seed <n>": a fixed random seed (randomize() then does nothing), so two runs of a harness draw the same.
static bool gHarnessSeeded = false;
static uint32_t gHarnessSeed = 0;
static u32 gKeysHeldPrev = 0;
static bool gExitRequested = false;
static N3DSChordButton gChordStart, gChordSelect;
static bool gChordLatched = false;
static bool gMonitorToggle = false;
static bool gStartComing = false;
static bool gRawStartPrev = false;
static bool gTouchHeld = false;
static int32_t gTouchX = 0, gTouchY = 0;

static u32 N3DSInput_keyFromName(const char* name) {
    static const struct { const char* name; u32 key; } names[] = {
        { "A", KEY_A }, { "B", KEY_B }, { "X", KEY_X }, { "Y", KEY_Y }, { "L", KEY_L }, { "R", KEY_R },
        { "ZL", KEY_ZL }, { "ZR", KEY_ZR }, { "START", KEY_START }, { "SELECT", KEY_SELECT },
        { "UP", KEY_DUP }, { "DOWN", KEY_DDOWN }, { "LEFT", KEY_DLEFT }, { "RIGHT", KEY_DRIGHT }, { "TOUCH", KEY_TOUCH },
    };
    repeat(sizeof(names) / sizeof(names[0]), i) {
        if (strcasecmp(names[i].name, name) == 0) return names[i].key;
    }
    return 0;
}

static void N3DSInput_loadHarness(void) {
    FILE* f = fopen(N3DS_SD_DIR "harness.txt", "r");
    if (f == NULL) return;
    char line[128];
    while (fgets(line, sizeof(line), f) != NULL) {
        char cmd[32] = "", arg[32] = "";
        int frame = 0, length = 1;
        char room[48] = "";
        unsigned seed = 0;
        if (sscanf(line, "seed %u", &seed) == 1) {
            gHarnessSeeded = true;
            gHarnessSeed = seed;
            continue;
        }
        if (sscanf(line, "after_room %47s", room) == 1) {
            N3DSHarnessAnchor a = { .frame = -1 };
            snprintf(a.room, sizeof(a.room), "%s", room);
            arrput(gAnchors, a);
            continue;
        }
        int tx = 0, ty = 0, tlen = 1, tx2 = -1, ty2 = -1;
        int tn = sscanf(line, "touch %d %d %d %d %d %d", &frame, &tx, &ty, &tlen, &tx2, &ty2);
        if (tn >= 3) {
            N3DSHarnessCmd c = { .kind = 3, .frame = frame, .length = tn >= 4 ? tlen : 1, .anchor = (int32_t) arrlen(gAnchors) - 1,
                .x = tx, .y = ty, .x2 = tn >= 6 ? tx2 : tx, .y2 = tn >= 6 ? ty2 : ty };
            arrput(gHarness, c);
            continue;
        }
        // "goto <frame> <room>": change to that room (like the desktop's debug Page Up/Down; persistent instances such
        // as Samus come along). "place <frame> <x> <y>": move Samus (oCharacter) there. For reaching a room to test.
        char gotoRoom[48] = "";
        if (sscanf(line, "goto %d %47s", &frame, gotoRoom) == 2) {
            N3DSHarnessCmd c = { .kind = 4, .frame = frame, .length = 1, .anchor = (int32_t) arrlen(gAnchors) - 1 };
            snprintf(c.room, sizeof(c.room), "%s", gotoRoom);
            arrput(gHarness, c);
            continue;
        }
        int px = 0, py = 0;
        if (sscanf(line, "place %d %d %d", &frame, &px, &py) == 3) {
            N3DSHarnessCmd c = { .kind = 5, .frame = frame, .length = 1, .anchor = (int32_t) arrlen(gAnchors) - 1, .x = px, .y = py };
            arrput(gHarness, c);
            continue;
        }
        int n = sscanf(line, "%31s %d %31s %d", cmd, &frame, arg, &length);
        if (n < 2 || cmd[0] == '#') continue;
        N3DSHarnessCmd c = { .frame = frame, .length = 1, .anchor = (int32_t) arrlen(gAnchors) - 1 };
        if (strcmp(cmd, "press") == 0 && n >= 3) {
            c.kind = 0;
            c.key = N3DSInput_keyFromName(arg);
            c.length = n >= 4 ? length : 1;
            if (c.key == 0) continue;
        } else if (strcmp(cmd, "screenshot") == 0) {
            c.kind = 1;
        } else if (strcmp(cmd, "exit") == 0) {
            c.kind = 2;
        } else {
            continue;
        }
        arrput(gHarness, c);
    }
    fclose(f);
    logInfo("Harness: %d commands from harness.txt\n", (int) arrlen(gHarness));
}

void N3DSInput_init(MAYBE_UNUSED Runner* runner) {
    N3DSInput_loadHarness();
    if (gHarnessSeeded && runner != NULL) {
        runner->vmContext->hasFixedSeed = true;
        Random_setSeed(&runner->random, gHarnessSeed);
        logInfo("Harness: random seed %lu\n", (unsigned long) gHarnessSeed);
    }
}

static void N3DSInput_fillGamepad(GamepadSlot* slot, u32 held, const circlePosition* circle, const circlePosition* cstick) {
    memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDownPrev));
    memset(slot->buttonDown, 0, sizeof(slot->buttonDown));
    memset(slot->buttonPressed, 0, sizeof(slot->buttonPressed));
    memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
    memset(slot->buttonValue, 0, sizeof(slot->buttonValue));
    memset(slot->axisValue, 0, sizeof(slot->axisValue));
    slot->connected = true;
    slot->jid = 0;
    snprintf(slot->description, sizeof(slot->description), "Nintendo 3DS");
    snprintf(slot->guid, sizeof(slot->guid), "n3ds-0");

    // GameMaker's face buttons are positions (gp_face1 = bottom, 2 = right, 3 = left, 4 = top), so by position:
    // B, A, Y, X. A game's own bindings then decide what each does (AM2R: res/n3ds/am2r/sd/config.ini).
    if (held & KEY_B) slot->buttonDown[0] = true;
    if (held & KEY_A) slot->buttonDown[1] = true;
    if (held & KEY_Y) slot->buttonDown[2] = true;
    if (held & KEY_X) slot->buttonDown[3] = true;
    if (held & KEY_L) slot->buttonDown[4] = true;
    if (held & KEY_R) slot->buttonDown[5] = true;
    slot->buttonValue[6] = (held & KEY_ZL) ? 1.0f : 0.0f;
    slot->buttonValue[7] = (held & KEY_ZR) ? 1.0f : 0.0f;
    if (held & KEY_ZL) slot->buttonDown[6] = true;
    if (held & KEY_ZR) slot->buttonDown[7] = true;
    if (held & KEY_SELECT) slot->buttonDown[8] = true;
    if (held & KEY_START) slot->buttonDown[9] = true;
    if (held & KEY_DUP) slot->buttonDown[12] = true;
    if (held & KEY_DDOWN) slot->buttonDown[13] = true;
    if (held & KEY_DLEFT) slot->buttonDown[14] = true;
    if (held & KEY_DRIGHT) slot->buttonDown[15] = true;
    slot->axisValue[0] = (float) circle->dx / 156.0f;
    slot->axisValue[1] = -(float) circle->dy / 156.0f;
    slot->axisValue[2] = (float) cstick->dx / 156.0f;
    slot->axisValue[3] = -(float) cstick->dy / 156.0f;
    repeat(GP_AXIS_COUNT, a) {
        if (slot->axisValue[a] > 1.0f) slot->axisValue[a] = 1.0f;
        if (slot->axisValue[a] < -1.0f) slot->axisValue[a] = -1.0f;
    }
    repeat(GP_BUTTON_COUNT, btn) {
        bool wasDown = slot->buttonDownPrev[btn];
        if (slot->buttonDown[btn] && !wasDown) slot->buttonPressed[btn] = true;
        if (!slot->buttonDown[btn] && wasDown) slot->buttonReleased[btn] = true;
    }
}

// Start and Select reach the game N3DS_CHORD_FRAMES late, press for press (a delay line): a chord of the two inside
// that window is dropped from both before the game sees either.
static bool N3DSInput_chordButton(N3DSChordButton* b, bool down) {
    b->history = (b->history << 1) | (down && !gChordLatched ? 1u : 0u);
    return ((b->history >> N3DS_CHORD_FRAMES) & 1u) != 0;
}

static u32 N3DSInput_filterChord(u32 held) {
    bool start = (held & KEY_START) != 0, select = (held & KEY_SELECT) != 0;
    if (start && select && !gChordLatched) {
        gChordLatched = true;
        gMonitorToggle = true;
        gChordStart.history = gChordSelect.history = 0;
    }
    gStartComing = start && !gRawStartPrev && !gChordLatched;
    gRawStartPrev = start;
    bool gameStart = N3DSInput_chordButton(&gChordStart, start);
    bool gameSelect = N3DSInput_chordButton(&gChordSelect, select);
    if (gChordLatched && !start && !select) gChordLatched = false;
    held &= ~(u32) (KEY_START | KEY_SELECT);
    if (gameStart) held |= KEY_START;
    if (gameSelect) held |= KEY_SELECT;
    return held;
}

// ===[ SELECT and START on the pause screen ]===
// The game has two pause entries:
//
//   Start  -> oControl's Start handler, which sets global.ssmode = 0 ... the map.
//   Escape -> oControl's VK_ESCAPE handler, which does the whole pause setup (camera, global.currentroom, ingame = 0,
//             transitiontype) and then sets global.ssmode = 3 ... the menu.
//
// Nothing on the 3DS sends Escape, so the port runs that handler instead and then picks the page: START lands on the
// menu it already opened, SELECT on the items page, which means switching global.ssmode and letting oSS_Control rebuild
// its page objects (user event 0 destroys them, user event 1 creates them for the current ssmode - the same pair
// oSubScrChange runs when the player changes page).
//
// To leave, the game's own exit is oSS_Fg's user event 0, which fades out and returns to global.currentroom. START
// already reaches that through the game's own binding; SELECT needs it called, since the game leaves SELECT unbound.
//
// All of this is the game's own code on the game's own state; nothing here simulates a press sequence.
#define N3DS_PAUSE_PAGE_ITEMS 1.0
#define N3DS_PAUSE_PAGE_MENU  3.0

static struct {
    double pendingPage;   // page the player asked for; -1 when there is nothing to do
    int32_t pausedFrames; // frames spent in the pause since, for the safety net
    // Buttons this front end has taken over and must keep from the game until they are let go. The pause room takes a
    // few frames to come up, and a tap that is still held when it does would otherwise reach the game's own Start
    // binding and close the pause on its first frame - the "opens and immediately closes" of a quick press.
    u32 swallowed;
} gPauseKeys = { -1.0, 0, 0 };

static Instance* N3DSInput_objectInstance(const char* name) {
    int32_t index = N3DSGml_objectIndex(name);
    return index >= 0 ? N3DSGml_firstInstance(index) : NULL;
}

static void N3DSInput_openPause(Runner* runner) {
    Instance* control = N3DSInput_objectInstance("oControl");
    if (control != NULL) Runner_executeEvent(runner, control, EVENT_KEYPRESS, 27); // VK_ESCAPE
}

static void N3DSInput_closePause(Runner* runner) {
    Instance* fg = N3DSInput_objectInstance("oSS_Fg");
    if (fg != NULL) Runner_executeEvent(runner, fg, EVENT_OTHER, 10); // user event 0: fade out, back to currentroom
}

// Whether the page the player asked for is the one on screen. The page objects are what tell: the menu is
// oPauseMenuOptions, the items page is oSubscreenMenu.
static bool N3DSInput_pageIsUp(double page) {
    if (page == N3DS_PAUSE_PAGE_MENU) return N3DSInput_objectInstance("oPauseMenuOptions") != NULL;
    if (page == N3DS_PAUSE_PAGE_ITEMS) return N3DSInput_objectInstance("oSubscreenMenu") != NULL;
    return true;
}

// Safety net. The page is normally chosen by writing global.ssmode before the room loads, so oSS_Control's Create builds
// it directly and nothing flashes. If the room loaded first and the page objects do not match after a moment, rebuild
// them here - the pair oSubScrChange runs when the player changes page.
static void N3DSInput_applyPendingPage(Runner* runner) {
    if (gPauseKeys.pendingPage < 0.0 || !N3DSPause_isPaused()) {
        gPauseKeys.pausedFrames = 0;
        return;
    }
    gPauseKeys.pausedFrames++;
    if (N3DSInput_pageIsUp(gPauseKeys.pendingPage)) {
        gPauseKeys.pendingPage = -1.0;
        gPauseKeys.pausedFrames = 0;
        return;
    }
    if (gPauseKeys.pausedFrames < 15) return; // give the room its frames to build the page first
    Instance* control = N3DSInput_objectInstance("oSS_Control");
    if (control == NULL) return;
    double page = gPauseKeys.pendingPage;
    gPauseKeys.pendingPage = -1.0;
    gPauseKeys.pausedFrames = 0;
    N3DSGml_setGlobal("ssmode", page);
    Runner_executeEvent(runner, control, EVENT_OTHER, 10);
    Runner_executeEvent(runner, control, EVENT_OTHER, 11);
    logInfo("Pause keys: rebuilt page %.0f after the room had loaded\n", page);
}

bool N3DSInput_startComing(void) {
    return gStartComing;
}

bool N3DSInput_takeMonitorToggle(void) {
    bool toggle = gMonitorToggle;
    gMonitorToggle = false;
    return toggle;
}

bool N3DSInput_touch(int32_t* x, int32_t* y) {
    *x = gTouchX;
    *y = gTouchY;
    return gTouchHeld;
}

void N3DSInput_injectButton(Runner* runner, int32_t index) {
    if (index < 0 || index >= GP_BUTTON_COUNT) return;
    GamepadSlot* slot = &runner->gamepads->slots[0];
    if (!slot->buttonDown[index] && !slot->buttonDownPrev[index]) slot->buttonPressed[index] = true;
    slot->buttonReleased[index] = false;
    slot->buttonDown[index] = true;
    slot->buttonValue[index] = 1.0f;
}

u32 N3DSInput_update(Runner* runner) {
    hidScanInput();
    u32 held = hidKeysHeld();
    touchPosition touch;
    hidTouchRead(&touch);
    gTouchHeld = (held & KEY_TOUCH) != 0;
    gTouchX = touch.px;
    gTouchY = touch.py;
    repeat(arrlen(gAnchors), a) {
        if (gAnchors[a].frame >= 0) continue;
        if (runner->currentRoom != NULL && runner->currentRoom->name != NULL && strcmp(runner->currentRoom->name, gAnchors[a].room) == 0) {
            gAnchors[a].frame = gFrame;
            logInfo("Harness: anchor %s at frame %d\n", gAnchors[a].room, (int) gFrame);
        }
        break; // anchors fire in order
    }
    repeat(arrlen(gHarness), i) {
        N3DSHarnessCmd* c = &gHarness[i];
        int32_t base = 0;
        if (c->anchor >= 0) {
            base = gAnchors[c->anchor].frame;
            if (base < 0) continue;
        }
        int32_t f = gFrame - base;
        if (c->kind == 0 && f >= c->frame && f < c->frame + c->length) held |= c->key;
        if (c->kind == 1 && f == c->frame) N3DSScreenshot_request(gFrame);
        if (c->kind == 2 && f == c->frame) gExitRequested = true;
        if (c->kind == 4 && f == c->frame) {
            int32_t roomIndex = N3DSGml_roomIndex(c->room);
            if (roomIndex >= 0) runner->pendingRoom = roomIndex;
            logInfo("Harness: goto %s (%d) at frame %d\n", c->room, (int) roomIndex, (int) gFrame);
        }
        if (c->kind == 5 && f == c->frame) {
            Instance* samus = N3DSGml_anyInstance(N3DSGml_objectIndex("oCharacter"));
            if (samus != NULL) {
                samus->x = (float) c->x;
                samus->y = (float) c->y;
            }
            logInfo("Harness: place Samus at %d,%d at frame %d%s\n", (int) c->x, (int) c->y, (int) gFrame, samus == NULL ? " (no oCharacter)" : "");
        }
        if (c->kind == 3 && f >= c->frame && f < c->frame + c->length) {
            float t = c->length > 1 ? (float) (f - c->frame) / (float) (c->length - 1) : 0.0f;
            gTouchHeld = true;
            gTouchX = c->x + (int32_t) ((float) (c->x2 - c->x) * t);
            gTouchY = c->y + (int32_t) ((float) (c->y2 - c->y) * t);
            held |= KEY_TOUCH;
        }
    }
    // In play, both buttons open the pause and pick a page. In the pause, SELECT leaves; START is left to the game,
    // whose own binding already exits. The pair still toggles the debug monitor. (The area description after a scan is
    // the game's own START binding, not SELECT: SELECT has no game-side use here.)
    {
        static u32 prevPauseKeys;
        u32 pauseKeys = held & (KEY_START | KEY_SELECT);
        u32 edges = pauseKeys & ~prevPauseKeys;
        prevPauseKeys = pauseKeys;
        if (edges != 0 && pauseKeys != (KEY_START | KEY_SELECT)) {
            if (N3DSPause_inPlay()) {
                double page = (edges & KEY_SELECT) ? N3DS_PAUSE_PAGE_ITEMS : N3DS_PAUSE_PAGE_MENU;
                N3DSInput_openPause(runner);
                // The handler ends by setting ssmode = 3 and asking for the room; both are undone by writing the page we
                // actually want now, so the room is built with it and never shows the menu first.
                N3DSGml_setGlobal("ssmode", page);
                gPauseKeys.pendingPage = page;
                gPauseKeys.swallowed |= edges;
            } else if (N3DSPause_isPaused() && (edges & KEY_SELECT) != 0) {
                N3DSInput_closePause(runner);
                gPauseKeys.pendingPage = -1.0;
                gPauseKeys.swallowed |= (edges & KEY_SELECT);
            }
        }
        // Keep a taken-over button away from the game until it is released, so the room's own bindings never see the
        // press that opened it.
        held &= ~(pauseKeys & gPauseKeys.swallowed);
        if (pauseKeys == 0) gPauseKeys.swallowed = 0;
    }
    N3DSInput_applyPendingPage(runner);
    held = N3DSInput_filterChord(held);
    circlePosition circle, cstick;
    hidCircleRead(&circle);
    hidCstickRead(&cstick);

    // Circle pad doubles as the D-pad for the keyboard keys.
    u32 dirs = held;
    if (circle.dx < -80) dirs |= KEY_DLEFT;
    if (circle.dx > 80) dirs |= KEY_DRIGHT;
    if (circle.dy > 80) dirs |= KEY_DUP;
    if (circle.dy < -80) dirs |= KEY_DDOWN;
#ifdef N3DS_KEYBOARD_MIRROR
    repeat(sizeof(gKeyMap) / sizeof(gKeyMap[0]), i) {
        bool now = (dirs & gKeyMap[i].key) != 0;
        bool before = (gKeysHeldPrev & gKeyMap[i].key) != 0;
        if (now && !before) RunnerKeyboard_onKeyDown(runner->keyboard, gKeyMap[i].gml);
        if (!now && before) RunnerKeyboard_onKeyUp(runner->keyboard, gKeyMap[i].gml);
    }
#endif
    u32 down = dirs & ~gKeysHeldPrev;
    gKeysHeldPrev = dirs;

    N3DSInput_fillGamepad(&runner->gamepads->slots[0], held, &circle, &cstick);
    runner->gamepads->connectedCount = 1;
    gFrame++;
    return down;
}

bool N3DSInput_exitRequested(void) {
    return gExitRequested;
}
