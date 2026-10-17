#include "n3ds_itemsscreen.h"

#include "n3ds_gml.h"
#include "n3ds_livemap.h"
#include "n3ds_renderer.h"

#include "../log.h"
#include "../rvalue.h"
#include "../utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// AM2R's items panel, drawn by this port from the game's own state.
//
// The game shows it on rm_subscreen, where its own oSubScr* objects draw four boxes around Samus. Those objects only
// exist in that room, so this rebuilds the same picture for the bottom screen: the same four boxes in the same
// places, the same rows, and the game's own globals deciding what is owned and what the description says.

// The screen's own geometry, measured off the game's 320x240 items page.
#define N3DS_ITEMS_W 320
#define N3DS_ITEMS_H 240
#define N3DS_ITEMS_STATUS_Y 0     // the energy/missile strip, flush with the top (the shared title bar)
#define N3DS_ITEMS_STATUS_H N3DS_BOTTOM_BAR_H
#define N3DS_ITEMS_ROW_H 9        // one item row
#define N3DS_ITEMS_HEADER_H 14    // the box's own title strip
#define N3DS_ITEMS_DESC_Y N3DS_BOTTOM_CONTENT_BOTTOM  // where the shared frame puts the footer
#define N3DS_ITEMS_DESC_H N3DS_BOTTOM_FOOTER_H  // the same height as the map page's footer
#define N3DS_ITEMS_HINT_Y 212     // the button hints

// Colours sampled from the game's page: the panel fill, its title strip, and the selected row.
#define N3DS_ITEMS_PANEL_COLOUR 0x061a06u
#define N3DS_ITEMS_HIGHLIGHT_COLOUR 0x1d5a0du

// Which item each row of each box shows. -1 is a row that is always there (the suit Samus starts in, the Morph
// Ball); the rest index global.item[], which the game sets as the items are collected.
typedef struct {
    int8_t item;
    const char* key;    // the [ItemsMenu] key, for the row's name
    const char* tipKey; // the [Items] key for its description; NULL when the two sections agree on a name
} N3DSItemRow;

typedef struct {
    const char* heading;  // the box's title, also an [ItemsMenu] key
    float x, y;
    int32_t width, height;
    const char* sprite;   // the box's background sprite
    int32_t rowCount;
    const N3DSItemRow* rows;
    int32_t firstRow;     // this box's first row in the flat selection order
} N3DSItemBox;

// [ItemsMenu] names these rows; [Items] holds their descriptions, and it does not always use the same word: the
// spring ball is SpringBall there and JumpBall here, the high-jump boots HiJumpBoots and HiJump.
static const N3DSItemRow kSuitRows[] = {
    { -1, "PowerSuit", NULL }, { 5, "VariaSuit", NULL }, { 9, "GravitySuit", NULL },
};
static const N3DSItemRow kBeamRows[] = {
    { 10, "ChargeBeam", NULL }, { 11, "IceBeam", NULL }, { 12, "WaveBeam", NULL }, { 13, "SpazerBeam", NULL },
    { 14, "PlasmaBeam", NULL },
};
static const N3DSItemRow kMiscRows[] = {
    { -1, "MorphBall", NULL }, { 2, "SpiderBall", NULL }, { 3, "SpringBall", "JumpBall" }, { 0, "Bombs", NULL },
    { 1, "PowerGrip", NULL }, { 8, "ScrewAttack", NULL },
};
static const N3DSItemRow kBootRows[] = {
    { 4, "HiJumpBoots", "HiJump" }, { 6, "SpaceJump", NULL }, { 7, "SpeedBooster", NULL },
};

// The four boxes, at the positions the game's page puts them. firstRow continues the flat selection order across the
// boxes (suits 0-2, beams 3-7, misc 8-13, boots 14-16).
static const N3DSItemBox kBoxes[] = {
    { "Suits", 20.0f, 20.0f, 67, 44, "sSubScrSuit", 3, kSuitRows, 0 },
    { "Beams", 20.0f, 108.0f, 67, 62, "sSubScrBeam", 5, kBeamRows, 3 },
    { "Misc", 233.0f, 20.0f, 67, 71, "sSubScrMisc", 6, kMiscRows, 8 },
    { "Boots", 233.0f, 126.0f, 67, 44, "sSubScrBoots", 3, kBootRows, 14 },
};
#define N3DS_ITEMS_BOX_COUNT ((int32_t) (sizeof(kBoxes) / sizeof(kBoxes[0])))
#define N3DS_ITEMS_ROW_COUNT 17

typedef struct {
    Runner* runner;
    Renderer* renderer;
    int32_t scriptText;  // get_text
    int32_t font;        // the body font
    int32_t sprPlayer;   // sSubScrPlayer, the figure in the middle
    int32_t sprItem;     // sSubScrItem, the row bullet
    int32_t sprHilight;  // sSubScrHilight, the marker on the selected row
    int32_t boxSprite[N3DS_ITEMS_BOX_COUNT];
    int32_t selected;    // which row is highlighted, as a flat index
    int32_t tipLogged;   // which row's tip was last logged, so it is said once per selection
    bool ready;
} N3DSItemsScreen;

static N3DSItemsScreen gItems;

void N3DSItemsScreen_init(Runner* runner, Renderer* renderer) {
    memset(&gItems, 0, sizeof(gItems));
    gItems.runner = runner;
    gItems.renderer = renderer;
    gItems.scriptText = N3DSGml_scriptIndex("get_text");
    // The fonts in data.win carry a "Default" suffix; the bare name is a fallback for other builds.
    gItems.font = N3DSGml_fontIndex("fontGUI2Default");
    if (gItems.font < 0) gItems.font = N3DSGml_fontIndex("fontGUI2");
    gItems.sprPlayer = N3DSGml_spriteIndex("sSubScrPlayer");
    gItems.sprItem = N3DSGml_spriteIndex("sSubScrItem");
    gItems.sprHilight = N3DSGml_spriteIndex("sSubScrHilight");
    for (int32_t i = 0; i < N3DS_ITEMS_BOX_COUNT; i++) gItems.boxSprite[i] = N3DSGml_spriteIndex(kBoxes[i].sprite);
    gItems.selected = 0;
    gItems.ready = gItems.font >= 0;
    if (!gItems.ready) logInfo("Items screen: no fontGUI2Default, the panel is off\n");
}

bool N3DSItemsScreen_available(void) {
    return gItems.ready;
}

// One string out of the language file, or NULL. The caller frees it.
static char* N3DSItemsScreen_text(const char* section, const char* key) {
    if (gItems.scriptText < 0) return NULL;
    // get_text(section, key): the section comes first.
    RValue args[2] = { RValue_makeString(section), RValue_makeString(key) };
    RValue result = N3DSGml_callScript(gItems.scriptText, args, 2);
    char* text = result.type == RVALUE_STRING && result.string != NULL ? safeStrdup(result.string) : NULL;
    RValue_free(&result);
    return text;
}

// Whether the game says an item is owned. -1 is a row that is always there.
static bool N3DSItemsScreen_owned(int8_t item) {
    if (item < 0) return true;
    double value = 0.0;
    if (!N3DSGml_getGlobal("item", item, &value)) return false;
    return value != 0.0;
}

static void N3DSItemsScreen_text1(const char* text, float x, float y, float scale) {
    if (text == NULL || text[0] == '\0') return;
    Renderer* r = gItems.renderer;
    r->drawFont = gItems.font;
    r->drawHalign = 0;
    r->drawAlpha = 1.0f;
    r->drawColor = 0x000000;
    r->vtable->drawText(r, text, x + 1.0f, y + 1.0f, scale, scale, 0.0f, -1.0f);
    r->drawColor = 0xFFFFFF;
    r->vtable->drawText(r, text, x, y, scale, scale, 0.0f, -1.0f);
}

static void N3DSItemsScreen_rect(float x0, float y0, float x1, float y1, uint32_t colour) {
    gItems.renderer->vtable->drawRectangle(gItems.renderer, x0, y0, x1, y1, colour, 1.0f, false);
}

// The same, but x/y are the sprite's top-left corner rather than its origin. The game's panel sprites put their
// origin at (0,33) and Samus at (0,48), so passing a corner straight through would move them up by that much - which
// is exactly what the first version of this page did.
static void N3DSItemsScreen_spriteAt(int32_t sprite, float left, float top, float alpha) {
    if (sprite < 0) return;
    DataWin* dataWin = gItems.runner->dataWin;
    if (dataWin == NULL || (uint32_t) sprite >= dataWin->sprt.count) return;
    Sprite* sp = &dataWin->sprt.sprites[sprite];
    Renderer_drawSpriteExt(gItems.renderer, sprite, 0, left + (float) sp->originX, top + (float) sp->originY,
        1.0f, 1.0f, 0.0f, 0xFFFFFF, alpha);
}

// The energy / missile strip along the top: the game's own numbers, laid out as its page does it.
static void N3DSItemsScreen_drawStatus(void) {
    char text[32];
    double health = 0.0, maxHealth = 0.0, missiles = 0.0, maxMissiles = 0.0;
    N3DSGml_getGlobal("playerhealth", -1, &health);
    N3DSGml_getGlobal("maxhealth", -1, &maxHealth);
    N3DSGml_getGlobal("missiles", -1, &missiles);
    N3DSGml_getGlobal("maxmissiles", -1, &maxMissiles);
    snprintf(text, sizeof(text), "%d/%d", (int) health, (int) maxHealth);
    N3DSItemsScreen_text1(text, 18.0f, (float) N3DS_ITEMS_STATUS_Y + 2.0f, 1.0f);
    snprintf(text, sizeof(text), "%d/%d", (int) missiles, (int) maxMissiles);
    N3DSItemsScreen_text1(text, 130.0f, (float) N3DS_ITEMS_STATUS_Y + 2.0f, 1.0f);
}

// One box: the panel, its title strip, and its rows.
static void N3DSItemsScreen_drawCallout(void);

static void N3DSItemsScreen_drawBox(int32_t index) {
    const N3DSItemBox* box = &kBoxes[index];
    // The game's own background when it is there, otherwise a plain panel of the same size.
    if (gItems.boxSprite[index] >= 0) {
        N3DSItemsScreen_spriteAt(gItems.boxSprite[index], box->x, box->y, 1.0f);
    } else {
        N3DSItemsScreen_rect(box->x, box->y, box->x + (float) box->width, box->y + (float) box->height,
            N3DS_ITEMS_PANEL_COLOUR);
    }
    // The panel sprite's own title strip is only about six pixels tall, which is not enough for a Chinese heading, so
    // the heading sits just under it and the rows start below that.
    char* heading = N3DSItemsScreen_text("ItemsMenu", box->heading);
    // Two pixels below the box top, cancelling the font's own -2 nudge so the heading stays where it was designed to
    // sit. The nudge is a whole-font one (fontGUI2 draws the game's numbers too) and this page cannot opt out of it.
    N3DSItemsScreen_text1(heading != NULL ? heading : box->heading, box->x + 3.0f, box->y + 2.0f, 1.0f);
    free(heading);

    for (int32_t row = 0; row < box->rowCount; row++) {
        const N3DSItemRow* item = &box->rows[row];
        float rowY = box->y + (float) N3DS_ITEMS_HEADER_H + (float) row * (float) N3DS_ITEMS_ROW_H;
        // Unowned items are not listed: the game's page leaves the row empty.
        if (!N3DSItemsScreen_owned(item->item)) continue;
        bool selected = gItems.selected == box->firstRow + row;
        if (selected) {
            N3DSItemsScreen_rect(box->x + 2.0f, rowY - 1.0f, box->x + (float) box->width - 2.0f,
                rowY + (float) N3DS_ITEMS_ROW_H - 2.0f, N3DS_ITEMS_HIGHLIGHT_COLOUR);
            // The game's own marker for the row it is on (sSubScrHilight, a 5x6 pixel block drawn round the name).
            if (gItems.sprHilight >= 0) {
                const float mark = 2.4f;
                Renderer_drawSpriteExt(gItems.renderer, gItems.sprHilight, 0, box->x + 3.0f, rowY + 1.0f,
                    mark, mark, 0.0f, 0xFFFFFF, 1.0f);
            }
        }
        // The bullet is 7x7 in the game's own page, which reads as a dot beside Chinese text one line tall; drawn at
        // 1.5x it matches the row height it is marking.
        if (gItems.sprItem >= 0) {
            DataWin* dataWin = gItems.runner->dataWin;
            if (dataWin != NULL && (uint32_t) gItems.sprItem < dataWin->sprt.count) {
                Sprite* bullet = &dataWin->sprt.sprites[gItems.sprItem];
                const float scale = 1.5f;
                Renderer_drawSpriteExt(gItems.renderer, gItems.sprItem, 0,
                    box->x + 4.0f + (float) bullet->originX * scale, rowY + (float) bullet->originY * scale,
                    scale, scale, 0.0f, 0xFFFFFF, 1.0f);
            }
        }
        char* label = N3DSItemsScreen_text("ItemsMenu", item->key);
        // Three-quarter size: the boxes are 67 wide, which fits the game's own English names (PLASMA BEAM) but not
        // their Chinese ones, where 等离子光束 is five full-width characters. At 0.75 the longest still fits.
        N3DSItemsScreen_text1(label != NULL ? label : item->key, box->x + 8.0f, rowY, 1.0f);
        free(label);
    }
}

// Samus in the middle, between the boxes. The game's own figure sprite, centred on the screen.
// Samus between the boxes. The figure is 201 tall, taller than the 164 the content band gives it, so it is scaled to
// fit rather than drawn over the bars.
static void N3DSItemsScreen_drawSamus(void) {
    if (gItems.sprPlayer < 0) return;
    DataWin* dataWin = gItems.runner->dataWin;
    if (dataWin == NULL || (uint32_t) gItems.sprPlayer >= dataWin->sprt.count) return;
    Sprite* sprite = &dataWin->sprt.sprites[gItems.sprPlayer];
    float w = (float) sprite->width;
    float h = (float) sprite->height;
    if (w <= 0.0f || h <= 0.0f) return;
    float band = (float) (N3DS_BOTTOM_CONTENT_BOTTOM - N3DS_BOTTOM_CONTENT_TOP);
    float scale = band / h;
    if (scale > 1.0f) scale = 1.0f;
    float drawW = w * scale;
    float drawH = h * scale;
    float left = ((float) N3DS_ITEMS_W - drawW) * 0.5f;
    float top = (float) N3DS_BOTTOM_CONTENT_TOP + (band - drawH) * 0.5f;
    Renderer_drawSpriteExt(gItems.renderer, gItems.sprPlayer, 0, left + (float) sprite->originX * scale,
        top + (float) sprite->originY * scale, scale, scale, 0.0f, 0xFFFFFF, 1.0f);
}

// The selected item's description, along the bottom, and the button hints under it.
// A line from the selected item to Samus, the way the game's own page points from a row to the figure so it is clear
// which item the description belongs to. Drawn from the row's inner edge towards the figure's centre; each box connects
// to the side that faces it.
static void N3DSItemsScreen_drawCallout(void) {
    const N3DSItemRow* item = NULL;
    const N3DSItemBox* box = NULL;
    for (int32_t b = 0; b < N3DS_ITEMS_BOX_COUNT && item == NULL; b++) {
        if (gItems.selected < kBoxes[b].firstRow || gItems.selected >= kBoxes[b].firstRow + kBoxes[b].rowCount) continue;
        box = &kBoxes[b];
        item = &box->rows[gItems.selected - box->firstRow];
    }
    if (item == NULL || !N3DSItemsScreen_owned(item->item)) return;
    // Only the rows that are actually listed have a line: an unowned row has nothing on screen to point from.
    float rowY = box->y + (float) N3DS_ITEMS_HEADER_H +
        (float) (gItems.selected - box->firstRow) * (float) N3DS_ITEMS_ROW_H + (float) N3DS_ITEMS_ROW_H * 0.5f;
    bool rightSide = box->x < (float) N3DS_ITEMS_W * 0.5f;
    float fromX = rightSide ? box->x + (float) box->width : box->x;
    float toX = (float) N3DS_ITEMS_W * 0.5f + (rightSide ? -14.0f : 14.0f);
    // Towards the figure's middle, so the line reads as pointing at Samus rather than at a corner.
    float toY = (float) ((N3DS_BOTTOM_CONTENT_TOP + N3DS_BOTTOM_CONTENT_BOTTOM) / 2);
    gItems.renderer->vtable->drawLine(gItems.renderer, fromX, rowY, toX, toY, 1.0f, 0x9fd0a0u, 1.0f);
}

static void N3DSItemsScreen_drawDescription(void) {
    const N3DSItemRow* item = NULL;
    for (int32_t b = 0; b < N3DS_ITEMS_BOX_COUNT && item == NULL; b++) {
        const N3DSItemBox* box = &kBoxes[b];
        if (gItems.selected < box->firstRow || gItems.selected >= box->firstRow + box->rowCount) continue;
        item = &box->rows[gItems.selected - box->firstRow];
    }

// The description is the <key>_Tip string - the same one the game's own items page shows under the boxes. It is
    // not <key>_Text: that is the in-play hint (how to use the item), a different sentence. The names above come from
    // [ItemsMenu] and the tips from [Items]; the two sections do not always agree on a name, so they are read
    // separately.
    char key[64];
    const char* tipKey = item != NULL ? (item->tipKey != NULL ? item->tipKey : item->key) : NULL;
    if (tipKey != NULL) snprintf(key, sizeof(key), "%s_Tip", tipKey);
    char* description = tipKey != NULL ? N3DSItemsScreen_text("Items", key) : NULL;
    // Diagnostic: say whether the tip came back, since an empty strip is otherwise indistinguishable from one that
    // was never drawn.
    if (gItems.tipLogged != gItems.selected) {
        gItems.tipLogged = gItems.selected;
        if (description == NULL) logInfo("Items screen: no tip for %s (looked up %s in [Items])\n",
            item != NULL ? item->key : "?", key);
        else logInfo("Items screen: tip for %s is [%.40s]\n", item != NULL ? item->key : "?", description);
    }
    if (description != NULL) {
        // The game marks line breaks with '#' and puts a button glyph where the text has a '|'. This port has no
        // glyph to put there, so the break becomes a space and the marker is dropped rather than shown as a bar.
        char* out = description;
        for (const char* p = description; *p != '\0'; p++) {
            if (*p == '#') *out++ = ' ';
            else if (*p == '|') continue;
            else *out++ = *p;
        }
        *out = '\0';
        // Ten-pixel text: the strip is 28 tall and the shared font draws larger than that at 1.0.
        const float tipScale = 10.0f / 12.0f;
        N3DSItemsScreen_text1(description, 6.0f, (float) N3DS_ITEMS_DESC_Y + 4.0f, tipScale);
        free(description);
    }
    // The hints the game's own items page puts along the bottom, in this port's terms.
    N3DSItemsScreen_text1("触摸选择道具", 4.0f, (float) N3DS_ITEMS_HINT_Y, 1.0f);
}

void N3DSItemsScreen_draw(void) {
    if (!gItems.ready) return;
    Renderer* r = gItems.renderer;
    // The game's draw state is left as it was.
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;

    // This page draws in 320x240 screen coordinates, so the bottom screen has to be made the target first - without
    // this the whole panel lands on the top screen, over the running game.
    N3DSRenderer_beginBottomScreenGUI(r, N3DS_ITEMS_W, N3DS_ITEMS_H);
    N3DSItemsScreen_rect(0.0f, 0.0f, (float) N3DS_ITEMS_W, (float) N3DS_ITEMS_H, 0x000000u);
    // Content first, bars second: anything a box or the figure draws past the content band is then covered by the
    // bars instead of spilling over them, which is what keeps every page to the same frame.
    N3DSItemsScreen_drawSamus();
    for (int32_t i = 0; i < N3DS_ITEMS_BOX_COUNT; i++) N3DSItemsScreen_drawBox(i);
    N3DSItemsScreen_drawCallout();
    N3DSLiveMap_drawBar((float) N3DS_ITEMS_STATUS_Y, (float) N3DS_ITEMS_STATUS_H);
    N3DSItemsScreen_drawStatus();
    N3DSLiveMap_drawBar((float) N3DS_ITEMS_DESC_Y, (float) N3DS_ITEMS_DESC_H);
    N3DSItemsScreen_drawDescription();
    N3DSLiveMap_drawButtons(0); // left slot: the items page
    N3DSRenderer_endBottomScreenGUI(r);

    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}

// Which row a screen point lands on: the boxes are laid out in a fixed grid, so this is the same walk the drawing
// does.
void N3DSItemsScreen_tap(int32_t x, int32_t y) {
    for (int32_t b = 0; b < N3DS_ITEMS_BOX_COUNT; b++) {
        const N3DSItemBox* box = &kBoxes[b];
        if ((float) x < box->x || (float) x >= box->x + (float) box->width) continue;
        if ((float) y < box->y + (float) N3DS_ITEMS_HEADER_H || (float) y >= box->y + (float) box->height) continue;
        int32_t row = (int32_t) (((float) y - box->y - (float) N3DS_ITEMS_HEADER_H) / (float) N3DS_ITEMS_ROW_H);
        if (row < 0 || row >= box->rowCount) return;
        if (!N3DSItemsScreen_owned(box->rows[row].item)) return;
        gItems.selected = box->firstRow + row;
        logInfo("Items screen: selected row %d (%s)\n", (int) gItems.selected, box->rows[row].key);
        return;
    }
}

int32_t N3DSItemsScreen_ownedCount(void) {
    int32_t count = 0;
    for (int32_t b = 0; b < N3DS_ITEMS_BOX_COUNT; b++) {
        for (int32_t row = 0; row < kBoxes[b].rowCount; row++) {
            if (N3DSItemsScreen_owned(kBoxes[b].rows[row].item)) count++;
        }
    }
    return count;
}
