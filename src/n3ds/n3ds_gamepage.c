#include "n3ds_gamepage.h"

#include "n3ds_gml.h"
#include "n3ds_renderer.h"

#include "../log.h"
#include "../rvalue.h"
#include "../vm.h"

#include <string.h>

// The game's own sub-screen pages, drawn through its own code.
//
// Each page is a set of objects whose Draw events do the work. Running those events with a surface bound as the
// target gives the page exactly as the game would draw it on rm_subscreen, which is why nothing here measures or
// recomputes a layout.
//
// The objects to run, in painting order (back first). Every one of them already exists in this game.
typedef struct {
    const char* object;
    uint32_t subtype; // the Draw sub-event: DRAW_NORMAL is the ordinary one
} N3DSGamePagePart;

// The items panel: the background, Samus, and the four boxes.
static const N3DSGamePagePart kItemsParts[] = {
    { "oSS_Bg", DRAW_NORMAL },
    { "oSubScrPlayer", DRAW_NORMAL },
    { "oSubScreenSuit", DRAW_NORMAL },
    { "oSubScreenBeam", DRAW_NORMAL },
    { "oSubScreenMisc", DRAW_NORMAL },
    { "oSubScreenBoots", DRAW_NORMAL },
    { "oSS_Fg", DRAW_NORMAL },
};

// The log screens: the background, the list, and the control strip.
static const N3DSGamePagePart kLogParts[] = {
    { "oSS_Bg", DRAW_NORMAL },
    { "oLogScreen", DRAW_NORMAL },
    { "oLogEntry", DRAW_NORMAL },
    { "oLogScreenControl", DRAW_NORMAL },
    { "oSS_Fg", DRAW_NORMAL },
};

// The live map stays this port's own drawing: it redraws from global.map every frame and follows Samus, which the
// game's pause map does not do while play carries on.
static const N3DSGamePagePart kMapParts[] = {
    { "oSS_Bg", DRAW_NORMAL },
    { "oSS_Fg", DRAW_NORMAL },
};

typedef struct {
    const N3DSGamePagePart* parts;
    int32_t partCount;
} N3DSGamePageSpec;

typedef struct {
    Runner* runner;
    Renderer* renderer;
    int32_t objControl;      // the instance used as `self`, alive from boot
    int32_t surface;         // the offscreen the game draws into, 0 until created
    int32_t objectIndex[N3DS_GAMEPAGE_COUNT][8];
    int32_t partCount[N3DS_GAMEPAGE_COUNT];
    bool ready;
} N3DSGamePage;

static N3DSGamePage gPage;

static const N3DSGamePageSpec kSpecs[N3DS_GAMEPAGE_COUNT] = {
    [N3DS_GAMEPAGE_ITEMS] = { kItemsParts, (int32_t) (sizeof(kItemsParts) / sizeof(kItemsParts[0])) },
    [N3DS_GAMEPAGE_MAP] = { kMapParts, (int32_t) (sizeof(kMapParts) / sizeof(kMapParts[0])) },
    [N3DS_GAMEPAGE_LOGS] = { kLogParts, (int32_t) (sizeof(kLogParts) / sizeof(kLogParts[0])) },
};

void N3DSGamePage_init(Runner* runner, Renderer* renderer) {
    memset(&gPage, 0, sizeof(gPage));
    gPage.runner = runner;
    gPage.renderer = renderer;
    gPage.objControl = N3DSGml_objectIndex("oControl");
    for (int32_t page = 0; page < N3DS_GAMEPAGE_COUNT; page++) {
        const N3DSGamePageSpec* spec = &kSpecs[page];
        gPage.partCount[page] = spec->partCount;
        for (int32_t i = 0; i < spec->partCount; i++) {
            gPage.objectIndex[page][i] = N3DSGml_objectIndex(spec->parts[i].object);
            if (gPage.objectIndex[page][i] < 0) {
                logInfo("Game page %d: no %s in this game, that page falls back\n", (int) page,
                    spec->parts[i].object);
            }
        }
    }
    gPage.ready = gPage.objControl >= 0;
    if (!gPage.ready) logInfo("Game pages: no oControl, they are off\n");
}

bool N3DSGamePage_available(void) {
    return gPage.ready;
}

// Runs one object's Draw event, with the instance the game already has for it as `self`. Several of these objects
// only exist in rm_subscreen; when there is no instance of one, its event is skipped rather than run without a self.
static bool N3DSGamePage_runDraw(int32_t objectIndex, uint32_t subtype) {
    if (objectIndex < 0 || gPage.runner == NULL || gPage.runner->vmContext == NULL) return false;
    DataWin* dataWin = gPage.runner->dataWin;
    if (dataWin == NULL || (uint32_t) objectIndex >= dataWin->objt.count) return false;
    GameObject* object = &dataWin->objt.objects[objectIndex];
    if (subtype >= OBJT_EVENT_TYPE_COUNT) return false;

    // The event's code, if this object has one.
    int32_t codeId = -1;
    ObjectEventList* list = &object->eventLists[subtype];
    for (uint32_t e = 0; e < list->eventCount; e++) {
        ObjectEvent* event = &list->events[e];
        if (event->eventSubtype != 0) continue;
        for (uint32_t a = 0; a < event->actionCount; a++) {
            if (event->actions[a].codeId >= 0) { codeId = event->actions[a].codeId; break; }
        }
        if (codeId >= 0) break;
    }
    if (codeId < 0) return false;

    // An instance to run it on. Without one, `self` would be something else entirely.
    Instance* instance = N3DSGml_anyInstance(objectIndex);
    if (instance == NULL) return false;

    VMContext* ctx = gPage.runner->vmContext;
    Instance* savedInstance = ctx->currentInstance;
    uint32_t savedEventType = ctx->currentEventType;
    ctx->currentInstance = instance;
    ctx->currentEventType = EVENT_DRAW;
    RValue result = VM_callCodeIndex(ctx, codeId, NULL, 0);
    RValue_free(&result);
    ctx->currentEventType = savedEventType;
    ctx->currentInstance = savedInstance;
    return true;
}

// Makes the offscreen surface the drawing target, runs the page's parts, then puts the target back.
static bool N3DSGamePage_drawInto(N3DSGamePage page) {
    if (gPage.surface == 0) {
        gPage.surface = Renderer_createSurface(gPage.renderer, 320, 240);
        if (gPage.surface == 0) {
            logInfo("Game pages: no surface, falling back\n");
            return false;
        }
        logInfo("Game pages: drawing into surface %d\n", (int) gPage.surface);
    }
    if (!Runner_surfaceSetTarget(gPage.runner, gPage.surface)) return false;
    // Black first, so whatever the parts do not cover is not last frame's picture.
    gPage.renderer->vtable->clearScreen(gPage.renderer, 0x000000, 1.0f);
    int32_t ran = 0;
    for (int32_t i = 0; i < gPage.partCount[page]; i++) {
        if (N3DSGamePage_runDraw(gPage.objectIndex[page][i], kSpecs[page].parts[i].subtype)) ran++;
    }
    Runner_surfaceResetTarget(gPage.runner);
    return ran > 0;
}

bool N3DSGamePage_draw(N3DSGamePage page) {
    if (!gPage.ready || page < 0 || page >= N3DS_GAMEPAGE_COUNT) return false;
    if (!N3DSGamePage_drawInto(page)) return false;
    // The surface is a 320x240 picture: the bottom-screen pass shows it 1:1.
    N3DSRenderer_beginBottomScreenGUI(gPage.renderer, 320, 240);
    gPage.renderer->drawSurface(gPage.renderer, gPage.surface, 0, 0, 320, 240, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f,
        0xFFFFFF, 1.0f);
    N3DSRenderer_endBottomScreenGUI(gPage.renderer);
    return true;
}
