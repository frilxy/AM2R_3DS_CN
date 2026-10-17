#include "n3ds_itemsscreen.h"

#include "n3ds_gml.h"
#include "n3ds_livemap.h"

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
#define N3DS_ITEMS_STATUS_Y 49    // the energy/missile strip
#define N3DS_ITEMS_STATUS_H 14
#define N3DS_ITEMS_ROW_H 9        // one item row
#define N3DS_ITEMS_HEADER_H 12    // the box's own title strip
#define N3DS_ITEMS_DESC_Y 216     // the description strip
#define N3DS_ITEMS_DESC_H 13
#define N3DS_ITEMS_HINT_Y 230     // the button hints

// Colours sampled from the game's page: the panel fill, its title strip, and the selected row.
#define N3DS_ITEMS_PANEL_COLOUR 0x061a06u
#define N3DS_ITEMS_BAR_COLOUR 0x3bab1bu
#define N3DS_ITEMS_HIGHLIGHT_COLOUR 0x1d5a0du

// Which item each row of each box shows. -1 is a row that is always there (the suit Samus starts in, the Morph
// Ball); the rest index global.item[], which the game sets as the items are collected.
typedef struct {
    int8_t item;
    const char* key; // the [ItemsMenu] key, for both the name and the description
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

static const N3DSItemRow kSuitRows[] = {
    { -1, "PowerSuit" }, { 5, "VariaSuit" }, { 9, "GravitySuit" },
};
static const N3DSItemRow kBeamRows[] = {
    { 10, "ChargeBeam" }, { 11, "IceBeam" }, { 12, "WaveBeam" }, { 13, "SpazerBeam" }, { 14, "PlasmaBeam" },
};
static const N3DSItemRow kMiscRows[] = {
    { -1, "MorphBall" }, { 2, "SpiderBall" }, { 3, "SpringBall" }, { 0, "Bombs" }, { 1, "PowerGrip" },
    { 8, "ScrewAttack" },
};
static const N3DSItemRow kBootRows[] = {
    { 4, "HiJumpBoots" }, { 6, "SpaceJump" }, { 7, "SpeedBooster" },
};

// The four boxes, at the positions the game's page puts them. firstRow continues the flat selection order across the
// boxes (suits 0-2, beams 3-7, misc 8-13, boots 14-16).
static const N3DSItemBox kBoxes[] = {
    { "Suits", 33.5f, 87.0f, 67, 44, "sSubScrSuit", 3, kSuitRows, 0 },
    { "Beams", 33.5f, 152.7f, 67, 62, "sSubScrBeam", 5, kBeamRows, 3 },
    { "Misc", 240.6f, 87.0f, 67, 71, "sSubScrMisc", 6, kMiscRows, 8 },
    { "Boots", 240.6f, 173.6f, 67, 44, "sSubScrBoots", 3, kBootRows, 14 },
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
    int32_t boxSprite[N3DS_ITEMS_BOX_COUNT];
    int32_t selected;    // which row is highlighted, as a flat index
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
    RValue args[2] = { RValue_makeString(key), RValue_makeString(section) };
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

static void N3DSItemsScreen_sprite(int32_t sprite, float x, float y, float alpha) {
    if (sprite < 0) return;
    Renderer_drawSpriteExt(gItems.renderer, sprite, 0, x, y, 1.0f, 1.0f, 0.0f, 0xFFFFFF, alpha);
}

// The energy / missile strip along the top: the game's own numbers, laid out as its page does it.
static void N3DSItemsScreen_drawStatus(void) {
    N3DSItemsScreen_rect(0.0f, (float) N3DS_ITEMS_STATUS_Y, (float) N3DS_ITEMS_W,
        (float) (N3DS_ITEMS_STATUS_Y + N3DS_ITEMS_STATUS_H), N3DS_ITEMS_BAR_COLOUR);
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
static void N3DSItemsScreen_drawBox(int32_t index) {
    const N3DSItemBox* box = &kBoxes[index];
    // The game's own background when it is there, otherwise a plain panel of the same size.
    if (gItems.boxSprite[index] >= 0) {
        N3DSItemsScreen_sprite(gItems.boxSprite[index], box->x, box->y, 1.0f);
    } else {
        N3DSItemsScreen_rect(box->x, box->y, box->x + (float) box->width, box->y + (float) box->height,
            N3DS_ITEMS_PANEL_COLOUR);
    }
    char* heading = N3DSItemsScreen_text("ItemsMenu", box->heading);
    N3DSItemsScreen_text1(heading != NULL ? heading : box->heading, box->x + 3.0f, box->y + 1.0f, 1.0f);
    free(heading);

    for (int32_t row = 0; row < box->rowCount; row++) {
        const N3DSItemRow* item = &box->rows[row];
        float rowY = box->y + (float) N3DS_ITEMS_HEADER_H + (float) row * (float) N3DS_ITEMS_ROW_H;
        // Unowned items are not listed: the game's page leaves the row empty.
        if (!N3DSItemsScreen_owned(item->item)) continue;
        if (gItems.selected == box->firstRow + row) {
            N3DSItemsScreen_rect(box->x + 2.0f, rowY - 1.0f, box->x + (float) box->width - 2.0f,
                rowY + (float) N3DS_ITEMS_ROW_H - 2.0f, N3DS_ITEMS_HIGHLIGHT_COLOUR);
        }
        N3DSItemsScreen_sprite(gItems.sprItem, box->x + 4.0f, rowY + 1.0f, 1.0f);
        char* label = N3DSItemsScreen_text("ItemsMenu", item->key);
        N3DSItemsScreen_text1(label != NULL ? label : item->key, box->x + 14.0f, rowY, 1.0f);
        free(label);
    }
}

// Samus in the middle, between the boxes. The game's own figure sprite, centred on the screen.
static void N3DSItemsScreen_drawSamus(void) {
    if (gItems.sprPlayer < 0) return;
    DataWin* dataWin = gItems.runner->dataWin;
    if (dataWin == NULL || (uint32_t) gItems.sprPlayer >= dataWin->sprt.count) return;
    int32_t w = (int32_t) dataWin->sprt.sprites[gItems.sprPlayer].width;
    N3DSItemsScreen_sprite(gItems.sprPlayer, (float) (N3DS_ITEMS_W / 2 - w / 2), 64.0f, 1.0f);
}

// The selected item's description, along the bottom, and the button hints under it.
static void N3DSItemsScreen_drawDescription(void) {
    const N3DSItemRow* item = NULL;
    for (int32_t b = 0; b < N3DS_ITEMS_BOX_COUNT && item == NULL; b++) {
        const N3DSItemBox* box = &kBoxes[b];
        if (gItems.selected < box->firstRow || gItems.selected >= box->firstRow + box->rowCount) continue;
        item = &box->rows[gItems.selected - box->firstRow];
    }
    N3DSItemsScreen_rect(0.0f, (float) N3DS_ITEMS_DESC_Y, (float) N3DS_ITEMS_W,
        (float) (N3DS_ITEMS_DESC_Y + N3DS_ITEMS_DESC_H), N3DS_ITEMS_BAR_COLOUR);
    char key[64];
    if (item != NULL) snprintf(key, sizeof(key), "%s_Text", item->key);
    char* description = item != NULL ? N3DSItemsScreen_text("ItemsMenu", key) : NULL;
    if (description != NULL) {
        // The game marks its line breaks with '#', as its own log and description text does.
        for (char* p = description; *p != '\0'; p++) {
            if (*p == '#') *p = ' ';
        }
        N3DSItemsScreen_text1(description, 6.0f, (float) N3DS_ITEMS_DESC_Y + 1.0f, 1.0f);
        free(description);
    }
    N3DSItemsScreen_text1("[^v] Select   [Z] Use   [X] Menu", 4.0f, (float) N3DS_ITEMS_HINT_Y, 1.0f);
}

void N3DSItemsScreen_draw(void) {
    if (!gItems.ready) return;
    Renderer* r = gItems.renderer;
    // The game's draw state is left as it was.
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;

    N3DSItemsScreen_rect(0.0f, 0.0f, (float) N3DS_ITEMS_W, (float) N3DS_ITEMS_H, 0x000000u);
    N3DSItemsScreen_drawStatus();
    N3DSItemsScreen_drawSamus();
    for (int32_t i = 0; i < N3DS_ITEMS_BOX_COUNT; i++) N3DSItemsScreen_drawBox(i);
    N3DSItemsScreen_drawDescription();
    N3DSLiveMap_drawButtons(0); // left slot: the items page

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
