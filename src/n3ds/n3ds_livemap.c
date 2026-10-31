#include "n3ds_livemap.h"

#include "n3ds_gml.h"
#include "n3ds_renderer.h"

#include "../gml_array.h"
#include "../log.h"
#include "../utils.h"

#include <math.h>
#include <string.h>

// What AM2R's map is made of (scripts draw_map_surf / draw_mapblock, the HUD's draw_gui_map):
//  - global.map[x, y]: "0" for no room, else 7 characters: walls up, right, down, left ("1" wall, "2" passage), block
//    colour ("1".."4" = sMapBlock 0..3), special mark (sMapSP), corner ("1".."9", "A".."Z", "a".."h" = sMapCorner 0..42).
//  - global.dmap[x, y]: 0 unexplored (not drawn), 1 explored, 2 mapped by a map station; 10 / 11 add sMapSP 6 / 7.
//  - A cell is 8x8; Samus is in cell global.mapposx, global.mapposy, shown by sMapHilight (alpha oControl.malpha);
//    global.mapmarker / mapmarkerx / mapmarkery is the player's marker (sMapMarker, frame oControl.markfr).
// Around it, the pause screen's map page (oSS_Fg / draw_surface_map): the bgMapScreenBG grid behind the cells
// (tiled from cell 3,3), black bands top and bottom, the bg_SubScrBottom bar with the page title, and the
// bg_MapBottom bar with the play time (as steps_to_time2(global.gametime)) and the Metroids left (global.monstersleft,
// global.monstersarea).
// The cells are redrawn only when they change: Samus changes cell, the marker moves, or a cell in view is explored
// (checked every few frames); the finished frame is kept as the bottom-screen picture for the frames in between. The
// bars, text and highlight are drawn on top of it every frame (the black bands cover the old ones), so the clock
// doesn't need a redraw. get_text reads the language file: the title is fetched once per language.

#define N3DS_LIVEMAP_CELL 8
#define N3DS_LIVEMAP_CHECK_FRAMES 15
#define N3DS_LIVEMAP_W 320
#define N3DS_LIVEMAP_H 240

typedef struct {
    Runner* runner;
    Renderer* renderer;
    int32_t sprBlock, sprCorner, sprHLine, sprVLine, sprHPass, sprVPass, sprSP, sprHilight, sprMarker;
    int32_t objControl;
    int32_t bgGrid, bgTopBar, bgBottomBar, font, scriptText;
    uint32_t frame;
    uint32_t lastDrawCall;
    uint32_t lastCheck;
    uint32_t signature; // of what the cells show (explored states in view, the marker)
    int32_t lastPx, lastPy;
    // The black bands cover everything the bars and text draw: the saved picture has the previous frame's in it.
    float topBandEnd, bottomBandStart;
    char* title;
    double titleLanguage;
    bool titleFetched;
    char time[16];
    int32_t timeSeconds;
    bool pendingCapture;
    bool haveSnapshot;
} N3DSLiveMap;

static N3DSLiveMap gLive;

void N3DSLiveMap_init(Runner* runner, Renderer* renderer) {
    free(gLive.title);
    memset(&gLive, 0, sizeof(gLive));
    gLive.timeSeconds = -1;
    gLive.runner = runner;
    gLive.renderer = renderer;
    gLive.sprBlock = N3DSGml_spriteIndex("sMapBlock");
    gLive.sprCorner = N3DSGml_spriteIndex("sMapCorner");
    gLive.sprHLine = N3DSGml_spriteIndex("sMapHLine");
    gLive.sprVLine = N3DSGml_spriteIndex("sMapVLine");
    gLive.sprHPass = N3DSGml_spriteIndex("sMapHPass");
    gLive.sprVPass = N3DSGml_spriteIndex("sMapVPass");
    gLive.sprSP = N3DSGml_spriteIndex("sMapSP");
    gLive.sprHilight = N3DSGml_spriteIndex("sMapHilight");
    gLive.sprMarker = N3DSGml_spriteIndex("sMapMarker");
    gLive.objControl = N3DSGml_objectIndex("oControl");
    gLive.bgGrid = N3DSGml_backgroundIndex("bgMapScreenBG");
    gLive.bgTopBar = N3DSGml_backgroundIndex("bg_SubScrBottom");
    gLive.bgBottomBar = N3DSGml_backgroundIndex("bg_MapBottom");
    gLive.font = N3DSGml_fontIndex("fontGUI2");
    gLive.scriptText = N3DSGml_scriptIndex("get_text");
    gLive.lastPx = gLive.lastPy = INT32_MIN;
    DataWin* dataWin = runner->dataWin;
    float barH = 0.0f, fontH = 0.0f;
    int32_t barTpag = gLive.bgTopBar >= 0 ? Renderer_resolveBackgroundTPAGIndex(dataWin, gLive.bgTopBar) : -1;
    if (barTpag >= 0 && (uint32_t) barTpag < dataWin->tpag.count) barH = (float) dataWin->tpag.items[barTpag].boundingHeight;
    if (gLive.font >= 0 && (uint32_t) gLive.font < dataWin->font.count) fontH = (float) dataWin->font.fonts[gLive.font].maxGlyphHeight;
    // The title at y 29 and the time at y 197 (+1 for the shadow), the top bar from y 30.
    gLive.topBandEnd = fmaxf(41.0f, fmaxf(30.0f + barH, 29.0f + fontH + 1.0f));
    gLive.bottomBandStart = 197.0f;
    if (gLive.sprBlock < 0 || gLive.sprCorner < 0 || gLive.sprHLine < 0 || gLive.sprVLine < 0 || gLive.sprHPass < 0 ||
        gLive.sprVPass < 0 || gLive.sprSP < 0) {
        logInfo("Live map: map sprites not in this game, off\n");
    }
}

static RValue* N3DSLiveMap_array(const char* name) {
    RValue* slot = N3DSGml_globalSlot(name);
    return slot != NULL && slot->type == RVALUE_ARRAY ? slot : NULL;
}

bool N3DSLiveMap_available(void) {
    if (gLive.sprBlock < 0 || gLive.sprCorner < 0 || gLive.sprHLine < 0 || gLive.sprVLine < 0 || gLive.sprHPass < 0 ||
        gLive.sprVPass < 0 || gLive.sprSP < 0) {
        return false;
    }
    double px, py;
    return N3DSLiveMap_array("map") != NULL && N3DSLiveMap_array("dmap") != NULL &&
        N3DSGml_getGlobal("mapposx", -1, &px) && N3DSGml_getGlobal("mapposy", -1, &py);
}

void N3DSLiveMap_beginFrame(void) {
    gLive.frame++;
    if (!gLive.pendingCapture) return;
    gLive.pendingCapture = false;
    gLive.haveSnapshot = N3DSRenderer_captureBottomSnapshot(gLive.renderer);
}

static void N3DSLiveMap_sprite(int32_t sprite, int32_t frame, float x, float y, float alpha) {
    if (sprite < 0) return;
    Renderer_drawSpriteExt(gLive.renderer, sprite, frame, x, y, 1.0f, 1.0f, 0.0f, 0xFFFFFF, alpha);
}

static int32_t N3DSLiveMap_cornerFrame(char c) {
    if (c >= '1' && c <= '9') return c - '1';
    if (c >= 'A' && c <= 'Z') return 9 + (c - 'A');
    if (c >= 'a' && c <= 'h') return 35 + (c - 'a');
    return -1;
}

// draw_mapblock, for one cell.
static void N3DSLiveMap_block(float x, float y, const char* cell, double explored) {
    if (explored <= 0.0) return;
    char wallU = cell[0], wallR = cell[1], wallD = cell[2], wallL = cell[3], color = cell[4], special = cell[5];
    char corner = cell[6];
    if (color >= '1' && color <= '4') N3DSLiveMap_sprite(gLive.sprBlock, color - '1', x, y, 1.0f);
    int32_t cornerFrame = N3DSLiveMap_cornerFrame(corner);
    if (cornerFrame >= 0) N3DSLiveMap_sprite(gLive.sprCorner, cornerFrame, x, y, 1.0f);
    if (wallU == '1') N3DSLiveMap_sprite(gLive.sprHLine, 0, x, y, 1.0f);
    if (wallD == '1') N3DSLiveMap_sprite(gLive.sprHLine, 0, x, y + 7.0f, 1.0f);
    if (wallL == '1') N3DSLiveMap_sprite(gLive.sprVLine, 0, x, y, 1.0f);
    if (wallR == '1') N3DSLiveMap_sprite(gLive.sprVLine, 0, x + 7.0f, y, 1.0f);
    if (wallU == '2') N3DSLiveMap_sprite(gLive.sprHPass, 0, x, y, 1.0f);
    if (wallD == '2') N3DSLiveMap_sprite(gLive.sprHPass, 0, x, y + 7.0f, 1.0f);
    if (wallL == '2') N3DSLiveMap_sprite(gLive.sprVPass, 0, x, y, 1.0f);
    if (wallR == '2') N3DSLiveMap_sprite(gLive.sprVPass, 0, x + 7.0f, y, 1.0f);
    int32_t sp = -1;
    switch (special) {
        case '1': sp = 0; break;
        case '2': sp = 1; break;
        case '3': sp = explored == 1.0 ? 2 : explored == 2.0 ? 5 : -1; break;
        case '4': sp = explored == 1.0 ? 2 : explored == 2.0 ? 3 : -1; break;
        case '5': sp = 4; break;
        case 'U': sp = 8; break;
        case 'D': sp = 9; break;
        case 'L': sp = 10; break;
        case 'R': sp = 11; break;
        case 'H': sp = 12; break;
        case 'V': sp = 13; break;
        case 'C': sp = 14; break;
        default: break;
    }
    if (sp >= 0) N3DSLiveMap_sprite(gLive.sprSP, sp, x, y, 1.0f);
    if (explored == 10.0) N3DSLiveMap_sprite(gLive.sprSP, 6, x, y, 1.0f);
    if (explored == 11.0) N3DSLiveMap_sprite(gLive.sprSP, 7, x, y, 1.0f);
}

static void N3DSLiveMap_cellOrigin(int32_t cx, int32_t cy, int32_t px, int32_t py, float* x, float* y) {
    *x = (float) (N3DS_LIVEMAP_W / 2 - N3DS_LIVEMAP_CELL / 2 + (cx - px) * N3DS_LIVEMAP_CELL);
    *y = (float) (N3DS_LIVEMAP_H / 2 - N3DS_LIVEMAP_CELL / 2 + (cy - py) * N3DS_LIVEMAP_CELL);
}

static void N3DSLiveMap_background(int32_t background, float x, float y, float xscale) {
    if (background < 0) return;
    int32_t tpag = Renderer_resolveBackgroundTPAGIndex(gLive.renderer->dataWin, background);
    if (tpag >= 0) gLive.renderer->vtable->drawSprite(gLive.renderer, tpag, x, y, 0.0f, 0.0f, xscale, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
}

// draw_text with a black shadow one pixel down-right, as the pause screen does.
static void N3DSLiveMap_text(const char* text, float x, float y, int32_t halign) {
    if (text == NULL || text[0] == '\0' || gLive.font < 0) return;
    Renderer* r = gLive.renderer;
    r->drawFont = gLive.font;
    r->drawHalign = halign;
    r->drawAlpha = 1.0f;
    r->drawColor = 0x000000;
    r->vtable->drawText(r, text, x + 1.0f, y + 1.0f, 1.0f, 1.0f, 0.0f, -1.0f);
    r->drawColor = 0xFFFFFF;
    r->vtable->drawText(r, text, x, y, 1.0f, 1.0f, 0.0f, -1.0f);
}

static char* N3DSLiveMap_callText(int32_t script, RValue* args, int32_t argCount) {
    RValue result = N3DSGml_callScript(script, args, argCount);
    char* text = result.type == RVALUE_STRING && result.string != NULL ? safeStrdup(result.string) : NULL;
    RValue_free(&result);
    return text;
}

static void N3DSLiveMap_twoDigits(char* out, size_t size, const char* global) {
    double value = 0.0;
    N3DSGml_getGlobal(global, -1, &value);
    snprintf(out, size, "%02d", (int) value);
}

static void N3DSLiveMap_drawFrame(void) {
    Renderer* r = gLive.renderer;
    // The game's draw state is left as it was.
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;
    r->vtable->drawRectangle(r, 0.0f, 0.0f, (float) N3DS_LIVEMAP_W, gLive.topBandEnd, 0x000000, 1.0f, false);
    r->vtable->drawRectangle(r, 0.0f, gLive.bottomBandStart, (float) N3DS_LIVEMAP_W, (float) N3DS_LIVEMAP_H, 0x000000, 1.0f, false);
    N3DSLiveMap_background(gLive.bgTopBar, 0.0f, 30.0f, 10.0f);
    N3DSLiveMap_background(gLive.bgBottomBar, 0.0f, 198.0f, 1.0f);
    double language = 0.0;
    N3DSGml_getGlobal("currentlanguage", -1, &language);
    // Once per language, whatever comes back (a failing call isn't retried every frame).
    if (gLive.scriptText >= 0 && (!gLive.titleFetched || language != gLive.titleLanguage)) {
        RValue titleArgs[2] = { RValue_makeString("Subscreen"), RValue_makeString("Title_Map") };
        free(gLive.title);
        gLive.title = N3DSLiveMap_callText(gLive.scriptText, titleArgs, 2);
        gLive.titleLanguage = language;
        gLive.titleFetched = true;
    }
    N3DSLiveMap_text(gLive.title != NULL ? gLive.title : "MAP", 160.0f, 29.0f, 1);
    double gameTime = 0.0;
    if (N3DSGml_getGlobal("gametime", -1, &gameTime)) {
        // steps_to_time2(gametime): gametime counts steps (60 a second), shown as HH:MM:SS. Done here, not by calling
        // the script: it keeps its working values in self's variables, and there is no self out here (each access
        // logged a warning, ~25 a call).
        int32_t seconds = (int32_t) (gameTime / 60.0);
        if (seconds != gLive.timeSeconds) {
            snprintf(gLive.time, sizeof(gLive.time), "%02d:%02d:%02d", (int) (seconds / 3600), (int) (seconds / 60 % 60), (int) (seconds % 60));
            gLive.timeSeconds = seconds;
        }
        N3DSLiveMap_text(gLive.time, 17.0f, 197.0f, 0);
    }
    char left[8], area[8];
    N3DSLiveMap_twoDigits(left, sizeof(left), "monstersleft");
    N3DSLiveMap_twoDigits(area, sizeof(area), "monstersarea");
    N3DSLiveMap_text(left, 259.0f, 197.0f, 0);
    N3DSLiveMap_text(area, 303.0f, 197.0f, 0);
    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}

static void N3DSLiveMap_drawCells(int32_t px, int32_t py) {
    RValue* map = N3DSLiveMap_array("map");
    RValue* dmap = N3DSLiveMap_array("dmap");
    if (map == NULL || dmap == NULL) return;
    if (gLive.bgGrid >= 0) {
        // The pause map's surface starts at cell 3,3 with the grid tiled from its corner.
        float gx, gy;
        N3DSLiveMap_cellOrigin(3, 3, px, py, &gx, &gy);
        int32_t tpag = Renderer_resolveBackgroundTPAGIndex(gLive.renderer->dataWin, gLive.bgGrid);
        if (tpag >= 0) {
            Renderer_drawBackgroundTiled(gLive.renderer, tpag, gx, gy, 1.0f, 1.0f, true, true, (float) N3DS_LIVEMAP_W,
                (float) N3DS_LIVEMAP_H, 0xFFFFFF, 1.0f);
        }
    }
    int32_t halfW = N3DS_LIVEMAP_W / N3DS_LIVEMAP_CELL / 2 + 1;
    int32_t halfH = N3DS_LIVEMAP_H / N3DS_LIVEMAP_CELL / 2 + 1;
    for (int32_t cx = px - halfW; cx <= px + halfW; cx++) {
        if (cx < 0) continue;
        for (int32_t cy = py - halfH; cy <= py + halfH; cy++) {
            if (cy < 0) continue;
            int32_t index = cx * GML_LEGACY_ARRAY_STRIDE + cy;
            RValue cell = GMLArray_getOnArrayRef(map, index);
            if (cell.type != RVALUE_STRING || cell.string == NULL || strlen(cell.string) < 7) continue;
            RValue explored = GMLArray_getOnArrayRef(dmap, index);
            if (explored.type == RVALUE_UNDEFINED || explored.type == RVALUE_STRING) continue;
            float x, y;
            N3DSLiveMap_cellOrigin(cx, cy, px, py, &x, &y);
            N3DSLiveMap_block(x, y, cell.string, (double) RValue_toReal(explored));
        }
    }
    double marker = 0.0, mx = 0.0, my = 0.0;
    if (gLive.sprMarker >= 0 && N3DSGml_getGlobal("mapmarker", -1, &marker) && marker != 0.0 &&
        N3DSGml_getGlobal("mapmarkerx", -1, &mx) && N3DSGml_getGlobal("mapmarkery", -1, &my)) {
        double markFrame = 0.0;
        Instance* control = N3DSGml_firstInstance(gLive.objControl);
        if (control != NULL) N3DSGml_getVar(control, "markfr", &markFrame);
        float x, y;
        N3DSLiveMap_cellOrigin((int32_t) mx, (int32_t) my, px, py, &x, &y);
        N3DSLiveMap_sprite(gLive.sprMarker, (int32_t) markFrame, x, y, 1.0f);
    }
}

// What the cells in view show: their explored states and the marker. A change means a redraw.
static uint32_t N3DSLiveMap_signature(int32_t px, int32_t py) {
    uint32_t h = 2166136261u;
    RValue* dmap = N3DSLiveMap_array("dmap");
    if (dmap != NULL) {
        int32_t halfW = N3DS_LIVEMAP_W / N3DS_LIVEMAP_CELL / 2 + 1;
        int32_t halfH = N3DS_LIVEMAP_H / N3DS_LIVEMAP_CELL / 2 + 1;
        for (int32_t cx = px - halfW; cx <= px + halfW; cx++) {
            if (cx < 0) continue;
            for (int32_t cy = py - halfH; cy <= py + halfH; cy++) {
                if (cy < 0) continue;
                RValue explored = GMLArray_getOnArrayRef(dmap, cx * GML_LEGACY_ARRAY_STRIDE + cy);
                int32_t v = explored.type == RVALUE_UNDEFINED || explored.type == RVALUE_STRING ? -1 : (int32_t) RValue_toReal(explored);
                h = (h ^ (uint32_t) v) * 16777619u;
            }
        }
    }
    double marker = 0.0, mx = 0.0, my = 0.0;
    N3DSGml_getGlobal("mapmarker", -1, &marker);
    N3DSGml_getGlobal("mapmarkerx", -1, &mx);
    N3DSGml_getGlobal("mapmarkery", -1, &my);
    h = (h ^ (uint32_t) (int32_t) marker) * 16777619u;
    h = (h ^ (uint32_t) (int32_t) mx) * 16777619u;
    h = (h ^ (uint32_t) (int32_t) my) * 16777619u;
    return h;
}

static void N3DSLiveMap_drawHighlight(void) {
    if (gLive.sprHilight < 0) return;
    double alpha = -1.0;
    Instance* control = N3DSGml_firstInstance(gLive.objControl);
    if (control == NULL || !N3DSGml_getVar(control, "malpha", &alpha)) {
        alpha = 0.5 + 0.5 * sin((double) gLive.frame * 0.15);
    }
    float x, y;
    N3DSLiveMap_cellOrigin(gLive.lastPx, gLive.lastPy, gLive.lastPx, gLive.lastPy, &x, &y);
    N3DSLiveMap_sprite(gLive.sprHilight, 0, x, y, (float) alpha);
}

void N3DSLiveMap_draw(void) {
    double pxd = 0.0, pyd = 0.0;
    if (!N3DSGml_getGlobal("mapposx", -1, &pxd) || !N3DSGml_getGlobal("mapposy", -1, &pyd)) return;
    int32_t px = (int32_t) pxd, py = (int32_t) pyd;
    // Redraw when Samus changes cell, when what the cells show changes, and after frames this didn't draw (pause
    // screen, menus): the bottom-screen picture may be something else's then.
    bool redraw = !gLive.haveSnapshot || px != gLive.lastPx || py != gLive.lastPy || gLive.lastDrawCall + 1 != gLive.frame;
    if (redraw || gLive.frame - gLive.lastCheck >= N3DS_LIVEMAP_CHECK_FRAMES) {
        uint32_t signature = N3DSLiveMap_signature(px, py);
        if (signature != gLive.signature) redraw = true;
        gLive.signature = signature;
        gLive.lastCheck = gLive.frame;
    }
    gLive.lastDrawCall = gLive.frame;
    gLive.lastPx = px;
    gLive.lastPy = py;

    if (!redraw) N3DSRenderer_drawBottomSnapshot(gLive.renderer);
    N3DSRenderer_beginBottomScreenGUI(gLive.renderer, N3DS_LIVEMAP_W, N3DS_LIVEMAP_H);
    if (redraw) {
        N3DSLiveMap_drawCells(px, py);
        // Kept as the picture from the next frame on, without the highlight (drawn live on top of it).
        gLive.pendingCapture = true;
    }
    N3DSLiveMap_drawFrame();
    if (!redraw) N3DSLiveMap_drawHighlight();
    N3DSRenderer_endBottomScreenGUI(gLive.renderer);
}
