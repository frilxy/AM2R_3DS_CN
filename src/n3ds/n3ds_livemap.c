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
// Everything is redrawn every frame. The dual-screen line kept the finished picture and blitted it back when nothing
// had changed, which needs a second screen-sized texture and the copy-to-texture path; a map view is a few hundred
// cells, so redrawing costs a fraction of a millisecond and there is no stale copy to get out of step. get_text reads
// the language file, so the title is fetched once per language.

// The bar colour, sampled from the game's own sub-screen page: rgb(8,58,8).
#define N3DS_BAR_COLOUR 0x083a08u

#define N3DS_LIVEMAP_CELL 8
#define N3DS_LIVEMAP_W 320
#define N3DS_LIVEMAP_H 240

// Vertical layout, from the top of the bottom screen down. The bands are the game's own map-page furniture, but the
// heights are this port's: the title bar and the information bar are squeezed to a line each so the map keeps the
// room, and the buttons get a strip of their own at the bottom.
#define N3DS_LIVEMAP_TITLE_H 16    // title bar (贴顶)
#define N3DS_LIVEMAP_INFO_H 20     // play time / items / Metroids row
#define N3DS_LIVEMAP_BUTTON_H 40   // the three touch buttons
#define N3DS_MAP_TOP N3DS_LIVEMAP_TITLE_H
#define N3DS_MAP_BOTTOM (N3DS_LIVEMAP_H - N3DS_LIVEMAP_BUTTON_H - N3DS_LIVEMAP_INFO_H)
#define N3DS_INFO_TOP N3DS_MAP_BOTTOM
#define N3DS_BUTTON_TOP (N3DS_LIVEMAP_H - N3DS_LIVEMAP_BUTTON_H)

// The bottom strip's three buttons, left to right: items, map, mission log. That is the game's own pause cross with
// its right, up and left arms laid out in a row, which is where the icons come from.
#define N3DS_MAP_BUTTON_COUNT N3DS_BUTTON_COUNT

typedef struct {
    Runner* runner;
    Renderer* renderer;
    int32_t sprBlock, sprCorner, sprHLine, sprVLine, sprHPass, sprVPass, sprSP, sprHilight, sprMarker;
    int32_t objControl;
    int32_t bgGrid, bgTopBar, bgBottomBar, font, scriptText;
    // One sprite per bottom button (items / map / log); -1 for one this game does not have.
    int32_t buttonSprite[N3DS_MAP_BUTTON_COUNT];
    uint32_t frame;
    int32_t lastPx, lastPy;
    // How far the player has dragged the map away from Samus, in pixels. Zero means the map follows her.
    float panX, panY;
    char* title;
    double titleLanguage;
    bool titleFetched;
    char time[16];
    int32_t timeSeconds;
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
    // The three buttons' icons, in left-to-right order: items, map, mission log.
    gLive.buttonSprite[0] = N3DSGml_spriteIndex("sItemVariaSuit");  // the suit, for the equipment page
    gLive.buttonSprite[1] = N3DSGml_spriteIndex("sMapMarkerMove");  // the marker, for the map
    // The fonts in data.win carry a "Default" suffix (fontGUI2Default, fontMenuTinyDefault, ...), so the bare
    // GameMaker name finds nothing and every map string is skipped - which is why the map had its bars and its
    // symbols but no title, no play time and no counter.
    gLive.font = N3DSGml_fontIndex("fontGUI2Default");
    if (gLive.font < 0) gLive.font = N3DSGml_fontIndex("fontGUI2");
    if (gLive.font < 0) logInfo("Live map: no fontGUI2Default in this game, the map's text is off\n");
    gLive.scriptText = N3DSGml_scriptIndex("get_text");
    gLive.lastPx = gLive.lastPy = INT32_MIN;
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

// The centre of the map area, not of the screen: the title bar and the button strip take their own rows, and the map
// has to be centred in what is left or Samus sits off-centre.
#define N3DS_MAP_CENTRE_X (N3DS_LIVEMAP_W / 2)
#define N3DS_MAP_CENTRE_Y (N3DS_MAP_TOP + (N3DS_MAP_BOTTOM - N3DS_MAP_TOP) / 2)

static void N3DSLiveMap_cellOrigin(int32_t cx, int32_t cy, int32_t px, int32_t py, float* x, float* y) {
    *x = (float) (N3DS_MAP_CENTRE_X - N3DS_LIVEMAP_CELL / 2 + (cx - px) * N3DS_LIVEMAP_CELL) + gLive.panX;
    *y = (float) (N3DS_MAP_CENTRE_Y - N3DS_LIVEMAP_CELL / 2 + (cy - py) * N3DS_LIVEMAP_CELL) + gLive.panY;
}

// Drags the map by a screen-space delta. The pan is clamped so the map cannot be flung off the screen: it may move
// one screenful away from Samus at most in either direction.
void N3DSLiveMap_panBy(float dx, float dy) {
    gLive.panX += dx;
    gLive.panY += dy;
    if (gLive.panX > (float) N3DS_LIVEMAP_W) gLive.panX = (float) N3DS_LIVEMAP_W;
    if (gLive.panX < -(float) N3DS_LIVEMAP_W) gLive.panX = -(float) N3DS_LIVEMAP_W;
    if (gLive.panY > (float) N3DS_LIVEMAP_H) gLive.panY = (float) N3DS_LIVEMAP_H;
    if (gLive.panY < -(float) N3DS_LIVEMAP_H) gLive.panY = -(float) N3DS_LIVEMAP_H;
}

// Puts the map back on Samus.
void N3DSLiveMap_resetPan(void) {
    gLive.panX = 0.0f;
    gLive.panY = 0.0f;
}

// A screen point turned into a map cell, with the pan taken into account. Used by the touch handling.
void N3DSLiveMap_screenToCell(int32_t sx, int32_t sy, int32_t* outX, int32_t* outY) {
    double pxd = 0.0, pyd = 0.0;
    if (!N3DSGml_getGlobal("mapposx", -1, &pxd) || !N3DSGml_getGlobal("mapposy", -1, &pyd)) return;
    float fx = (float) sx - gLive.panX;
    float fy = (float) sy - gLive.panY;
    *outX = (int32_t) pxd + (int32_t) floorf((fx - (float) N3DS_MAP_CENTRE_X) / (float) N3DS_LIVEMAP_CELL);
    *outY = (int32_t) pyd + (int32_t) floorf((fy - (float) N3DS_MAP_CENTRE_Y) / (float) N3DS_LIVEMAP_CELL);
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

void N3DSLiveMap_drawButtons(int32_t current);

static void N3DSLiveMap_drawFrame(void) {
    Renderer* r = gLive.renderer;
    // The game's draw state is left as it was.
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;

    // Three bands across the bottom screen: the title at the very top, the map in the middle, the counters and the
    // buttons below it. The black behind them is what the game's own map page uses; the map area is left clear.
    r->vtable->drawRectangle(r, 0.0f, (float) N3DS_INFO_TOP, (float) N3DS_LIVEMAP_W, (float) N3DS_LIVEMAP_H,
        0x000000, 1.0f, false);
    // The game's own bars, squeezed into the new heights (they were drawn for a taller layout). drawBar is the same
    // call the items and log pages make, so all three bars are the same picture.
    N3DSLiveMap_drawBar(0.0f, (float) N3DS_LIVEMAP_TITLE_H);
    N3DSLiveMap_drawBar((float) N3DS_INFO_TOP, (float) N3DS_LIVEMAP_INFO_H);
    // The game's own bottom bar over the flat colour: it carries the clock, the item and the Metroid glyphs the row's
    // numbers sit beside, which a plain rectangle has no way to show.
    if (gLive.bgBottomBar >= 0) {
        int32_t tpag = Renderer_resolveBackgroundTPAGIndex(r->dataWin, gLive.bgBottomBar);
        if (tpag >= 0 && (uint32_t) tpag < r->dataWin->tpag.count) {
            float bw = (float) r->dataWin->tpag.items[tpag].boundingWidth;
            float bh = (float) r->dataWin->tpag.items[tpag].boundingHeight;
            if (bw > 0.0f && bh > 0.0f) {
                // One scale for both axes, taken from the height: the bar has to be exactly the strip's height, so that
                // is the scale that cannot bend the glyphs. Stretching x and y by different amounts (320 across by 20
                // down) squashed the Metroid and item marks - they are only a few pixels wide to begin with. The flat
                // colour underneath already runs the full width; this only puts the glyphs back at the left.
                float scale = (float) N3DS_LIVEMAP_INFO_H / bh;
                r->vtable->drawSprite(r, tpag, 0.0f, (float) N3DS_INFO_TOP, 0.0f, 0.0f, scale, scale, 0.0f,
                    0xFFFFFF, 1.0f);
            }
        }
    }

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
    N3DSLiveMap_text(gLive.title != NULL ? gLive.title : "MAP", (float) (N3DS_LIVEMAP_W / 2), 2.0f, 1);

    // The counters sit on the information row's own baseline.
    const float infoY = (float) N3DS_INFO_TOP + 2.0f;
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
        N3DSLiveMap_text(gLive.time, 17.0f, infoY, 0);
    }
    char left[8], area[8], collected[8];
    N3DSLiveMap_twoDigits(left, sizeof(left), "monstersleft");
    N3DSLiveMap_twoDigits(area, sizeof(area), "monstersarea");
    // Items collected, as the percentage the game keeps in global.percent.
    double percent = 0.0;
    N3DSGml_getGlobal("percent", -1, &percent);
    snprintf(collected, sizeof(collected), "%d%%", (int) percent);
    N3DSLiveMap_text(collected, 227.0f, infoY, 0);
    N3DSLiveMap_text(left, 259.0f, infoY, 0);
    N3DSLiveMap_text(area, 303.0f, infoY, 0);

    N3DSLiveMap_drawButtons(1); // the map page is slot 1 (left items, middle map, right log)

    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}

// The three touch buttons. Left to right they open the items panel, the map and the mission log - the same three the
// game's own pause cross offers right, up and left. Each is a panel with the game's own icon in it.

// The green bar across the top of a page, and the same bar in the middle of the log page. The game draws these from
// bg_SubScrBottom stretched sideways, but that background is a small tile whose own width makes the stretch ratio
// fragile - it came out half-width on the log page. A flat rectangle of the colour sampled from the game's page is
// exact, one draw call, and cannot be a fraction of the screen wide.
void N3DSLiveMap_drawBar(float y, float height) {
    gLive.renderer->vtable->drawRectangle(gLive.renderer, 0.0f, y, (float) N3DS_LIVEMAP_W, y + height,
        N3DS_BAR_COLOUR, 1.0f, false);
}

// The button strip along the bottom edge, drawn on every page; `current` is the one whose page is showing, which
// gets a brighter border so the bottom screen says where it is.
void N3DSLiveMap_drawButtons(int32_t current) {
    Renderer* r = gLive.renderer;
    const float slotW = (float) N3DS_LIVEMAP_W / (float) N3DS_BUTTON_COUNT;
    const float top = (float) N3DS_BOTTOM_BUTTON_TOP;
    const float h = (float) N3DS_BOTTOM_BUTTON_H;
    // The two pages' names, in the order the strip lays them out.
    static const char* const names[N3DS_BUTTON_COUNT] = { "萨姆斯", "地图" };
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t colour = r->drawColor;
    float alpha = r->drawAlpha;
    for (int32_t i = 0; i < N3DS_BUTTON_COUNT; i++) {
        float x = (float) i * slotW;
        bool active = i == current;
        // A dark panel with a lighter top edge; the active one is brighter, so the strip says which page is up.
        r->vtable->drawRectangle(r, x + 4.0f, top + 3.0f, x + slotW - 4.0f, top + h - 3.0f,
            active ? 0x1d3a10u : 0x12200a, 1.0f, false);
        r->vtable->drawRectangle(r, x + 4.0f, top + 3.0f, x + slotW - 4.0f, top + 4.0f,
            active ? 0x5fae2eu : 0x2f5c14, 1.0f, false);
        // The page's name centred in the button, with a dark copy behind it like every other label on these pages.
        if (gLive.font >= 0) {
            r->drawFont = gLive.font;
            r->drawHalign = 1;
            r->drawAlpha = 1.0f;
            float cx = x + slotW * 0.5f;
            float cy = top + h * 0.5f - 6.0f;
            r->drawColor = 0x000000;
            r->vtable->drawText(r, names[i], cx + 1.0f, cy + 1.0f, 1.0f, 1.0f, 0.0f, -1.0f);
            r->drawColor = active ? 0xffffffu : 0x8fb890u;
            r->vtable->drawText(r, names[i], cx, cy, 1.0f, 1.0f, 0.0f, -1.0f);
        }
    }
    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = colour;
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
    gLive.lastPx = px;
    gLive.lastPy = py;
    // Redrawn every frame. The dual-screen line kept a copy of the finished picture and blitted it back on frames
    // where nothing had changed, which needs a second 320x240 texture and the copy-to-texture path; this port redraws
    // the few hundred cells instead, which is a fraction of a millisecond and has nothing to keep in sync.
    N3DSRenderer_beginBottomScreenGUI(gLive.renderer, N3DS_LIVEMAP_W, N3DS_LIVEMAP_H);
    N3DSLiveMap_drawCells(px, py);
    N3DSLiveMap_drawFrame();
    N3DSLiveMap_drawHighlight();
    N3DSRenderer_endBottomScreenGUI(gLive.renderer);
}
