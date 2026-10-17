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

#define N3DS_LIVEMAP_CELL 8
#define N3DS_LIVEMAP_W 320
#define N3DS_LIVEMAP_H 240

// The map's zoom steps, as the pixel height of one map cell: 0.5x, 1x, 2x and 3x against the cells' native 8.
// 8 and 16 and 24 are whole multiples, so their upscales are pixel-exact under a nearest filter; 4 is the only
// downscale and the renderer switches to a linear filter there, where a 1-pixel wall line thins to a half-pixel
// and the map reads as blockier than at 1x.
static const int32_t kZoomSteps[] = { 4, 8, 16, 24 };
#define N3DS_ZOOM_COUNT ((int32_t) (sizeof(kZoomSteps) / sizeof(kZoomSteps[0])))
#define N3DS_ZOOM_DEFAULT 1  // 8 pixels a cell, the native size

// The two rows at the top of the bottom screen. The green bar does not move: it stays where the game's map page
// puts it (y 30..44) and the counters are drawn on it. What moves is the title, off the bar and into the black
// margin above it, so the bar carries live numbers and the page's name sits in the space that was empty.
//
// The title is 12 pixels tall, so it centres in the 30-pixel margin at (30 - 12) / 2 = 9.
#define N3DS_TITLE_BAR_TOP 30.0f   // the green bar keeps its place; the counters go on it
#define N3DS_STATUS_BAND_BOTTOM 44.0f
#define N3DS_STATUS_TITLE_Y 9.0f   // the title's own top, centred in the margin above the bar
#define N3DS_STATUS_ICON_H 12
#define N3DS_STATUS_GAP 2       // between an icon and its value
#define N3DS_STATUS_MIN_GAP 4.0f  // the least space between two entries, whatever the counters' widths
// The counters' row on the green bar: the icons are 12 pixels tall (the game's ammo sprites) and the bar is 14, so
// centring them puts their top at 31.
#define N3DS_STATUS_Y (N3DS_TITLE_BAR_TOP + 1.0f)
// The values are drawn two pixels above the icons. A line of the font's digits is only 10 tall, so they sit higher in
// the bar than the 12-pixel icons do; 29..38 with the shadow at 30..39 stays inside the bar, which ends at 44.
#define N3DS_STATUS_TEXT_Y (N3DS_TITLE_BAR_TOP - 1.0f)
// The value font is fontGUI2: digits are 6 wide with shift 7, so a rendered digit is 7 pixels.
#define N3DS_STATUS_DIGIT_W 7
// The energy icon: there is no stand-alone sprite for the green "E" plate the game's own HUD shows (its plate is a
// black 33x20 background, coloured at draw time), so the port draws the plate itself. It sits on the bar, so it is
// the brighter green the bar's highlight uses rather than the bar's own dark fill, or it would vanish into it.
//
// Colours here are BGR, not RGB (the renderer unpacks them with BGR_R/BGR_G/BGR_B), so a value read as RGB off a
// screenshot has its red and blue swapped: 0x84e629 pasted in straight draws cyan instead of green.
// 6x5, measured off a console screenshot of the game's own bar: at that bar's scale the plate is 19x16 screenshot
// pixels against a 45-pixel-tall bar, and the bar is 14 game pixels, so the plate is about 6x5 in game pixels.
#define N3DS_STATUS_ETANK_W 6
#define N3DS_STATUS_ETANK_H 5
// The E is a vertical gradient, which is what the reference has: a light green top stroke, a mid green middle and a
// dark green bottom. Literals are BGR (see the note above).
#define N3DS_STATUS_ETANK_TOP 0x63e67bu    // RGB 0x7be663
#define N3DS_STATUS_ETANK_MID 0x00d629u    // RGB 0x29d600
#define N3DS_STATUS_ETANK_BOT 0x009400u    // RGB 0x009400
#define N3DS_STATUS_ETANK_BACK 0x000000u   // the counter behind the E, which is black rather than the bar's green
// The ammo sprites are white on the game's own black plate; on the green bar a multiplicative tint in the plate's
// green puts them in the same family as the energy plate next to them.
#define N3DS_STATUS_ICON_TINT N3DS_STATUS_ETANK_MID

// The zoom control in the bottom margin. The game's footer bar ends at 212, so the control owns 212..240: 28 pixels,
// which is enough for one line of the 12-pixel font with room above and below.
#define N3DS_STATUS_ZOOM_TOP 212.0f
#define N3DS_STATUS_ZOOM_LABEL_Y 220.0f
#define N3DS_ZOOM_DIM_COLOUR 0x5a5a5au  // the grey the settings pages use for a value that is not selected

typedef struct {
    Runner* runner;
    Renderer* renderer;
    int32_t sprBlock, sprCorner, sprHLine, sprVLine, sprHPass, sprVPass, sprSP, sprHilight, sprMarker;
    int32_t objControl;
    int32_t bgGrid, bgTopBar, bgBottomBar, font, scriptText;
    // The status strip's ammo icons: the game's own sprites (sGUIMissile, sGUISMissile, sGUIPBomb). The energy entry
    // has no sprite to borrow - the game's green plate is a black background coloured at draw time - so it is drawn
    // from rectangles instead (N3DSLiveMap_drawEnergyPlate).
    int32_t sprMissile, sprSMissile, sprPBomb;
    uint32_t frame;
    int32_t lastPx, lastPy;
    // How far the player has dragged the map away from Samus, in pixels. Zero means the map follows her.
    float panX, panY;
    // Which zoom step (an index into kZoomSteps) the map is drawn at.
    int32_t zoom;
    // The black bands cover everything the bars and text draw: the saved picture has the previous frame's in it.
    float topBandEnd, bottomBandStart;
    char* title;
    double titleLanguage;
    bool titleFetched;
    char time[16];
    int32_t timeSeconds;
} N3DSLiveMap;

static N3DSLiveMap gLive;

// The cell size the map is currently drawn at.
static int32_t N3DSLiveMap_cell(void) {
    if (gLive.zoom < 0) return kZoomSteps[0];
    if (gLive.zoom >= N3DS_ZOOM_COUNT) return kZoomSteps[N3DS_ZOOM_COUNT - 1];
    return kZoomSteps[gLive.zoom];
}

// How much a cell's 8x8 sprites are scaled at the current step.
static float N3DSLiveMap_cellScale(void) {
    return (float) N3DSLiveMap_cell() / (float) N3DS_LIVEMAP_CELL;
}

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
    gLive.sprMissile = N3DSGml_spriteIndex("sGUIMissile");
    gLive.sprSMissile = N3DSGml_spriteIndex("sGUISMissile");
    gLive.sprPBomb = N3DSGml_spriteIndex("sGUIPBomb");
    gLive.zoom = N3DS_ZOOM_DEFAULT;
    // The fonts in data.win carry a "Default" suffix (fontGUI2Default, fontMenuTinyDefault, ...), so the bare
    // GameMaker name finds nothing and every map string is skipped - which is why the map had its bars and its
    // symbols but no title, no play time and no counter.
    gLive.font = N3DSGml_fontIndex("fontGUI2Default");
    if (gLive.font < 0) gLive.font = N3DSGml_fontIndex("fontGUI2");
    if (gLive.font < 0) logInfo("Live map: no fontGUI2Default in this game, the map's text is off\n");
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
}

static void N3DSLiveMap_sprite(int32_t sprite, int32_t frame, float x, float y, float alpha) {
    if (sprite < 0) return;
    float scale = N3DSLiveMap_cellScale();
    Renderer_drawSpriteExt(gLive.renderer, sprite, frame, x, y, scale, scale, 0.0f, 0xFFFFFF, alpha);
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
    float edge = (float) N3DSLiveMap_cell() - 1.0f;
    if (wallD == '1') N3DSLiveMap_sprite(gLive.sprHLine, 0, x, y + edge, 1.0f);
    if (wallL == '1') N3DSLiveMap_sprite(gLive.sprVLine, 0, x, y, 1.0f);
    if (wallR == '1') N3DSLiveMap_sprite(gLive.sprVLine, 0, x + edge, y, 1.0f);
    if (wallU == '2') N3DSLiveMap_sprite(gLive.sprHPass, 0, x, y, 1.0f);
    if (wallD == '2') N3DSLiveMap_sprite(gLive.sprHPass, 0, x, y + edge, 1.0f);
    if (wallL == '2') N3DSLiveMap_sprite(gLive.sprVPass, 0, x, y, 1.0f);
    if (wallR == '2') N3DSLiveMap_sprite(gLive.sprVPass, 0, x + edge, y, 1.0f);
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
    float cell = (float) N3DSLiveMap_cell();
    *x = ((float) N3DS_LIVEMAP_W * 0.5f - cell * 0.5f + (float) (cx - px) * cell) + gLive.panX;
    *y = ((float) N3DS_LIVEMAP_H * 0.5f - cell * 0.5f + (float) (cy - py) * cell) + gLive.panY;
}

// Declared ahead of their definitions: the zoom control below is drawn before the text helpers it uses are defined.
static float N3DSLiveMap_statusTextWidth(const char* text);
static void N3DSLiveMap_textColoured(const char* text, float x, float y, int32_t halign, uint32_t colour);

// The zoom control along the bottom margin: one "[1X]" per step, the current step highlighted. The brackets follow
// the settings pages' convention for a selected value, and the enabled/disabled colouring is theirs too.
static void N3DSLiveMap_drawZoomControl(void) {
    // The step's scale against the cells' native 8 pixels, upper case to match the settings pages' option text.
    static const char* kLabels[N3DS_ZOOM_COUNT] = { "0.5X", "1X", "2X", "3X" };
    Renderer* r = gLive.renderer;
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;
    r->drawFont = gLive.font;
    r->drawAlpha = 1.0f;
    // Each step owns an equal slice of the strip, so a tap lands on the step whose label is under the finger.
    float slice = (float) N3DS_LIVEMAP_W / (float) N3DS_ZOOM_COUNT;
    for (int32_t i = 0; i < N3DS_ZOOM_COUNT; i++) {
        bool current = i == gLive.zoom;
        char label[24];
        // The brackets are the settings pages' mark for the value in effect.
        snprintf(label, sizeof(label), "%s%s%s", current ? "[" : " ", kLabels[i], current ? "]" : " ");
        // Unselected steps are dimmed rather than hidden, the way the settings pages grey out rows that cannot be
        // edited; the current one is drawn in the bright text colour.
        float centre = slice * (float) i + slice * 0.5f;
        float width = N3DSLiveMap_statusTextWidth(label);
        N3DSLiveMap_textColoured(label, centre - width * 0.5f, N3DS_STATUS_ZOOM_LABEL_Y, 0,
            current ? 0xFFFFFF : N3DS_ZOOM_DIM_COLOUR);
    }
    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}

// A tap on the zoom strip: -1 when the point is not on it.
int32_t N3DSLiveMap_zoomAt(int32_t sx, int32_t sy) {
    if (sy < (int32_t) N3DS_STATUS_ZOOM_TOP || sy > (int32_t) N3DS_LIVEMAP_H) return -1;
    if (sx < 0 || sx >= N3DS_LIVEMAP_W) return -1;
    int32_t slice = N3DS_LIVEMAP_W / N3DS_ZOOM_COUNT;
    int32_t step = sx / slice;
    if (step < 0) step = 0;
    if (step >= N3DS_ZOOM_COUNT) step = N3DS_ZOOM_COUNT - 1;
    return step;
}

void N3DSLiveMap_setZoom(int32_t step) {
    if (step < 0) step = 0;
    if (step >= N3DS_ZOOM_COUNT) step = N3DS_ZOOM_COUNT - 1;
    if (step == gLive.zoom) return;
    gLive.zoom = step;
    logInfo("Live map: zoom %dpx a cell\n", (int) N3DSLiveMap_cell());
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
    float cell = (float) N3DSLiveMap_cell();
    *outX = (int32_t) pxd + (int32_t) floorf((fx - (float) N3DS_LIVEMAP_W * 0.5f) / cell);
    *outY = (int32_t) pyd + (int32_t) floorf((fy - (float) N3DS_LIVEMAP_H * 0.5f) / cell);
}

static void N3DSLiveMap_background(int32_t background, float x, float y, float xscale) {
    if (background < 0) return;
    int32_t tpag = Renderer_resolveBackgroundTPAGIndex(gLive.renderer->dataWin, background);
    if (tpag >= 0) gLive.renderer->vtable->drawSprite(gLive.renderer, tpag, x, y, 0.0f, 0.0f, xscale, 1.0f, 0.0f, 0xFFFFFF, 1.0f);
}

// draw_text with a black shadow one pixel down-right, as the pause screen does.
static void N3DSLiveMap_textColoured(const char* text, float x, float y, int32_t halign, uint32_t colour) {
    if (text == NULL || text[0] == '\0' || gLive.font < 0) return;
    Renderer* r = gLive.renderer;
    r->drawFont = gLive.font;
    r->drawHalign = halign;
    r->drawAlpha = 1.0f;
    r->drawColor = 0x000000;
    r->vtable->drawText(r, text, x + 1.0f, y + 1.0f, 1.0f, 1.0f, 0.0f, -1.0f);
    r->drawColor = colour;
    r->vtable->drawText(r, text, x, y, 1.0f, 1.0f, 0.0f, -1.0f);
}

// The usual white-on-black-shadow line.
static void N3DSLiveMap_text(const char* text, float x, float y, int32_t halign) {
    N3DSLiveMap_textColoured(text, x, y, halign, 0xFFFFFF);
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

// The status strip: energy, missiles, super missiles and power bombs, each as its icon and "value/max".
//
// It is laid out from measured widths rather than fixed columns. A counter grows as the game goes on (999/999 is
// seven characters, not five), and fixed columns would either collide or leave the strip off centre, so the row is
// built in two passes: measure every entry, then start at (320 - total) / 2 and walk right.

typedef struct {
    int32_t icon;      // sprite index, or -1 for an entry with no icon
    float iconScale;   // the game's icons are drawn at 1:1; the plate is drawn, and uses the plate constants
    char text[24];
    float width;       // icon + gap + text, filled in by the measuring pass
    bool visible;      // false for an item the player does not have yet
    bool drawn;        // true for the energy plate, which is rectangles rather than a sprite
} N3DSStatusEntry;

#define N3DS_STATUS_ENTRY_COUNT 4

// Digits in fontGUI2 are 6 wide with shift 7 (the game advances 7), and '/' is the same, so a value's width is
// simply its length. Anything that is not a digit or a slash (a ten-pixel '%', say) is not used here.
// How wide a string draws in the map's font. Summed from each glyph's own advance rather than counted per character:
// in fontGUI2 a digit is 7 pixels wide but '1' and '.' are 3, and the brackets are 5, so a fixed 7 is wrong for any
// string with those in it - which is every zoom label ("0.75x" came out far too wide and the steps overlapped).
static float N3DSLiveMap_statusTextWidth(const char* text) {
    if (text == NULL || gLive.font < 0) return 0.0f;
    DataWin* dataWin = gLive.runner->dataWin;
    if (dataWin == NULL || (uint32_t) gLive.font >= dataWin->font.count) return 0.0f;
    Font* font = &dataWin->font.fonts[gLive.font];
    float width = 0.0f;
    for (const char* c = text; *c != '\0'; c++) {
        // The kerning table's shiftModifier is not applied: the map's own strings are digits and punctuation, and the
        // game's HUD uses the same plain advance.
        const FontGlyph* glyph = ((unsigned char) *c) < 128u ? font->glyphLUT[(unsigned char) *c] : NULL;
        // A glyph with no advance in this font still takes a small step, so text cannot collapse onto itself.
        width += glyph != NULL && glyph->shift > 0 ? (float) glyph->shift : (float) N3DS_STATUS_DIGIT_W;
    }
    return width;
}

// The drawn size of an entry's icon. Measured from the sprite, not assumed: the ammo icons are 16x12 while the
// energy plate is drawn rather than looked up, so its size comes from the constants.
static void N3DSLiveMap_statusIconSize(const N3DSStatusEntry* entry, float* outW, float* outH) {
    *outW = 0.0f;
    *outH = 0.0f;
    if (entry->drawn) {
        *outW = (float) N3DS_STATUS_ETANK_W;
        *outH = (float) N3DS_STATUS_ETANK_H;
        return;
    }
    if (entry->icon < 0) return;
    DataWin* dataWin = gLive.runner->dataWin;
    if (dataWin == NULL || (uint32_t) entry->icon >= dataWin->sprt.count) return;
    Sprite* sprite = &dataWin->sprt.sprites[entry->icon];
    *outW = (float) sprite->width * entry->iconScale;
    *outH = (float) sprite->height * entry->iconScale;
}

// The energy plate: a green square with a black "E" cut into it, the way the game's own HUD marks the bar. There is
// no sprite for it in data.win (the game's plate is a black background coloured at draw time), so it is drawn here
// from rectangles: the block, then the three strokes of the E.
static void N3DSLiveMap_drawEnergyPlate(float x, float top) {
    Renderer* r = gLive.renderer;
    const float w = (float) N3DS_STATUS_ETANK_W, h = (float) N3DS_STATUS_ETANK_H;
    // A black tile with a green E on it. One pixel per feature: top stroke, spine, middle stroke, spine, bottom
    // stroke, which is exactly the 5 rows the reference plate has.
    r->vtable->drawRectangle(r, x, top, x + w, top + h, N3DS_STATUS_ETANK_BACK, 1.0f, false);
    // The spine runs the full height and is two pixels wide, as the reference's is.
    r->vtable->drawRectangle(r, x, top, x + 2.0f, top + h, N3DS_STATUS_ETANK_MID, 1.0f, false);
    // Top, middle and bottom arms, each spanning the plate. Drawn after the spine so they cross it.
    r->vtable->drawRectangle(r, x, top, x + w, top + 1.0f, N3DS_STATUS_ETANK_TOP, 1.0f, false);
    r->vtable->drawRectangle(r, x, top + 2.0f, x + w, top + 3.0f, N3DS_STATUS_ETANK_MID, 1.0f, false);
    r->vtable->drawRectangle(r, x, top + h - 1.0f, x + w, top + h, N3DS_STATUS_ETANK_BOT, 1.0f, false);
}

// value/max. An item whose capacity is still zero (no missiles yet, no power bombs) has not been found, and the
// strip leaves it out rather than showing "0/0" beside the ones the player does have.
static void N3DSLiveMap_statusPair(N3DSStatusEntry* entry, const char* current, const char* maximum) {
    double now = 0.0, cap = 0.0;
    if (!N3DSGml_getGlobal(current, -1, &now)) now = 0.0;
    if (!N3DSGml_getGlobal(maximum, -1, &cap)) cap = 0.0;
    snprintf(entry->text, sizeof(entry->text), "%d/%d", (int) now, (int) cap);
    entry->visible = cap > 0.0;
}

static void N3DSLiveMap_drawStatus(void) {
    Renderer* r = gLive.renderer;
    N3DSStatusEntry entries[N3DS_STATUS_ENTRY_COUNT];
    memset(entries, 0, sizeof(entries));
    for (int32_t i = 0; i < N3DS_STATUS_ENTRY_COUNT; i++) entries[i].visible = true;
    entries[0].icon = -1;  // drawn as a plate, not a sprite: see N3DS_STATUS_ETANK_*
    entries[0].drawn = true;
    entries[0].iconScale = 1.0f;
    entries[1].icon = gLive.sprMissile;
    entries[1].iconScale = 1.0f;
    entries[2].icon = gLive.sprSMissile;
    entries[2].iconScale = 1.0f;
    entries[3].icon = gLive.sprPBomb;
    entries[3].iconScale = 1.0f;
    // playerhealth is what the HUD shows (the items page adds maxhealth beside it); maxhealth is not among the
    // globals the game declares for its HUD, so a build without it falls back to showing the value alone. Energy is
    // always shown: it cannot be un-found.
    double health = 0.0, maxHealth = 0.0;
    N3DSGml_getGlobal("playerhealth", -1, &health);
    if (N3DSGml_getGlobal("maxhealth", -1, &maxHealth) && maxHealth > 0.0) {
        snprintf(entries[0].text, sizeof(entries[0].text), "%d/%d", (int) health, (int) maxHealth);
    } else {
        snprintf(entries[0].text, sizeof(entries[0].text), "%d", (int) health);
    }
    N3DSLiveMap_statusPair(&entries[1], "missiles", "maxmissiles");
    N3DSLiveMap_statusPair(&entries[2], "smissiles", "maxsmissiles");
    N3DSLiveMap_statusPair(&entries[3], "pbombs", "maxpbombs");

    float total = 0.0f;
    int32_t shown = 0;
    for (int32_t i = 0; i < N3DS_STATUS_ENTRY_COUNT; i++) {
        float iconW, iconH;
        N3DSLiveMap_statusIconSize(&entries[i], &iconW, &iconH);
        (void) iconH;
        entries[i].width = iconW + (iconW > 0.0f ? (float) N3DS_STATUS_GAP : 0.0f) +
            N3DSLiveMap_statusTextWidth(entries[i].text);
        if (entries[i].visible) total += entries[i].width;
        shown += entries[i].visible ? 1 : 0;
    }
    if (shown == 0) return;
    // The gaps share whatever the entries leave, counting only the ones actually drawn. With every counter at three
    // digits this is about 25 pixels, which is what separates the groups on the game's own strip.
    float gap = 0.0f;
    if (total < (float) N3DS_LIVEMAP_W) gap = ((float) N3DS_LIVEMAP_W - total) / (float) (shown + 1);
    // The row is centred while the gaps stay at least N3DS_STATUS_MIN_GAP; past that the groups would read as one run
    // of digits, so the gaps are held at the floor and the row starts at the left edge instead. Holding the floor and
    // still centring would push the last entry off the screen.
    float start = gap;
    if (gap < N3DS_STATUS_MIN_GAP) {
        gap = N3DS_STATUS_MIN_GAP;
        start = 0.0f;
    }

    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;
    r->drawFont = gLive.font;
    r->drawHalign = 0;
    r->drawAlpha = 1.0f;
    r->drawColor = 0xFFFFFF;
    float x = start;
    int32_t drawn = 0;
    for (int32_t i = 0; i < N3DS_STATUS_ENTRY_COUNT; i++) {
        if (!entries[i].visible) continue;
        float iconW, iconH;
        N3DSLiveMap_statusIconSize(&entries[i], &iconW, &iconH);
        if (iconW > 0.0f) {
            // The icons keep their own row; the values were lifted two pixels above it, so the two are placed
            // independently instead of off one shared baseline. Rounded down: the energy plate is 5 pixels tall
            // against a 12-pixel row, and half a pixel would leave it straddling two rows of the screen.
            float iconTop = floorf(N3DS_STATUS_Y + (float) (N3DS_STATUS_ICON_H - iconH) * 0.5f);
            if (entries[i].drawn) {
                N3DSLiveMap_drawEnergyPlate(x, iconTop);
            } else {
                DataWin* dataWin = gLive.runner->dataWin;
                Sprite* sprite = &dataWin->sprt.sprites[entries[i].icon];
                // Tinted green like the energy plate. The ammo sprites are near-white on the game's own black HUD
                // plate; left white on the green bar they read as a different family from the plate beside them, so
                // they are multiplied by the plate's green instead. The tint is multiplicative, so the sprites' white
                // becomes that green and their darker pixels stay darker - the shapes survive.
                Renderer_drawSpriteExt(r, entries[i].icon, 0,
                    x + (float) sprite->originX * entries[i].iconScale,
                    iconTop + (float) sprite->originY * entries[i].iconScale,
                    entries[i].iconScale, entries[i].iconScale, 0.0f, N3DS_STATUS_ICON_TINT, 1.0f);
            }
            x += iconW + (float) N3DS_STATUS_GAP;
        }
        float textW = N3DSLiveMap_statusTextWidth(entries[i].text);
        N3DSLiveMap_text(entries[i].text, x, N3DS_STATUS_TEXT_Y, 0);
        x += textW;
        // The gap goes between the entries that are drawn, not after the last one and not around the hidden ones.
        drawn++;
        if (drawn < shown) x += gap;
    }
    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}

static void N3DSLiveMap_drawFrame(void) {
    Renderer* r = gLive.renderer;
    // The game's draw state is left as it was.
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;
    // The green bar keeps its place at y 30; the title moves up into the black margin above it and the counters are
    // drawn on the bar itself, so the bar carries the live numbers instead of the page's name.
    r->vtable->drawRectangle(r, 0.0f, 0.0f, (float) N3DS_LIVEMAP_W, N3DS_STATUS_BAND_BOTTOM, 0x000000, 1.0f, false);
    r->vtable->drawRectangle(r, 0.0f, gLive.bottomBandStart, (float) N3DS_LIVEMAP_W, (float) N3DS_LIVEMAP_H, 0x000000, 1.0f, false);
    N3DSLiveMap_background(gLive.bgTopBar, 0.0f, N3DS_TITLE_BAR_TOP, 10.0f);
    // On the bar, so after it: the counters are readable against the green, and the bar is not drawn over them.
    N3DSLiveMap_drawStatus();
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
    // In the black margin above the bar, centred there.
    N3DSLiveMap_text(gLive.title != NULL ? gLive.title : "MAP", 160.0f, N3DS_STATUS_TITLE_Y, 1);
    // The bottom margin's zoom steps.
    N3DSLiveMap_drawZoomControl();
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
    char left[8], area[8], collected[8];
    N3DSLiveMap_twoDigits(left, sizeof(left), "monstersleft");
    N3DSLiveMap_twoDigits(area, sizeof(area), "monstersarea");
    // Items collected, as the percentage the game keeps in global.percent.
    double percent = 0.0;
    N3DSGml_getGlobal("percent", -1, &percent);
    snprintf(collected, sizeof(collected), "%d%%", (int) percent);
    N3DSLiveMap_text(collected, 227.0f, 197.0f, 0);
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
    int32_t cell = N3DSLiveMap_cell();
    int32_t halfW = N3DS_LIVEMAP_W / cell / 2 + 1;
    int32_t halfH = N3DS_LIVEMAP_H / cell / 2 + 1;
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
