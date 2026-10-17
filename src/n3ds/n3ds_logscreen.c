#include "n3ds_logscreen.h"

#include "n3ds_gml.h"
#include "n3ds_livemap.h"

#include "../log.h"
#include "../rvalue.h"
#include "../utils.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// AM2R's log screens, drawn by this port from the game's own state.
//
// The game shows them on rm_subscreen, where oLogScreen and its entries are objects that only exist in that room.
// This rebuilds the same two pages for the bottom screen. The table below is the game's own: read out of
// gml_Script_load_logs_list, which lists every entry in display order with the index it occupies in global.log[].
// That index is also what decides its picture, bgLogImgNN.

#define N3DS_LOG_W 320
#define N3DS_LOG_H 240
#define N3DS_LOG_TITLE_Y 49     // the title strip
#define N3DS_LOG_TITLE_H 14
#define N3DS_LOG_BODY_Y 63      // the list starts here
#define N3DS_LOG_FOOT_Y 216     // the footer strip
#define N3DS_LOG_FOOT_H 14
#define N3DS_LOG_ROW_H 16       // one entry in the list
#define N3DS_LOG_CAT_H 20       // a category heading, with its highlight bar
#define N3DS_LOG_LIST_W 170     // the list column; the picture takes the rest
#define N3DS_LOG_TEXT_L 12.0f   // where an entry's text starts
#define N3DS_LOG_PIC_X 170
#define N3DS_LOG_DESC_LINE 11   // one line of an entry's body

#define N3DS_LOG_BAR_COLOUR 0x2f7a12u
#define N3DS_LOG_SELECT_COLOUR 0x1d5a0du
#define N3DS_LOG_LOCKED_COLOUR 0x3a4a3au

// One entry: the index it occupies in global.log[], the [Logs] key for its title and text, and which section that key
// lives in. The game builds the same list in load_logs_list; the order here is its order.
typedef struct {
    int8_t index;
    const char* key;
    bool federation; // its strings are in [NewLogs] rather than [Logs]
} N3DSLogEntry;

typedef struct {
    const char* heading; // the category's [LogsCategories] key
    bool federation;     // an [NewLogsCategories] category
    int32_t first;       // first entry in kEntries
    int32_t count;
} N3DSLogCategory;

static const N3DSLogEntry kEntries[] = {
    // Intel (情报)
    { 0, "Briefing", false },
    { 1, "Planet", false },
    { 3, "Earthquakes", false },
    { 4, "ResearchTeam", false },
    { 5, "RescueTeam", false },
    { 6, "Thoth", false },
    // Monsters (密特罗德)
    { 10, "Monsters", false },
    { 11, "Mutations", false },
    { 12, "AlphaMonster", false },
    { 13, "GammaMonster", false },
    { 14, "ZetaMonster", false },
    { 15, "OmegaMonster", false },
    { 16, "QueenMonster", false },
    // TheChozo (鸟人族)
    { 20, "Chozo", false },
    { 21, "GoldenTemple", false },
    { 22, "BreedingGrounds", false },
    { 23, "HydroStation", false },
    { 24, "IndustrialComplex", false },
    { 25, "MiningFacility", false },
    { 26, "Tower", false },
    { 27, "PowerPlant", false },
    { 28, "PowerDistribution", false },
    { 29, "GeneticLabs", false },
    { 30, "NativeSpecies", false },
    { 32, "AncientGuardian", false },
    { 33, "Arachnus", false },
    { 34, "Torizo", false },
    { 35, "Tester", false },
    { 36, "TankPrototype", false },
    { 37, "Serris", false },
    { 40, "OmegaNest", false },
    // Federation (银河联邦)
    { 38, "Genesis", true },
    { 41, "Nuclei", true },
    { 42, "Adaptations", true },
    { 43, "Radioactivity", true },
    { 44, "ChozoMessage", true },
    { 45, "ChozoMessage2", true },
};
#define N3DS_LOG_ENTRY_COUNT ((int32_t) (sizeof(kEntries) / sizeof(kEntries[0])))

// The four categories the game registers (its fifth, Hostiles, has no entries in this game).
static const N3DSLogCategory kCategories[] = {
    { "Intel", false, 0, 6 },
    { "Monsters", false, 6, 7 },
    { "TheChozo", false, 13, 18 },
    { "Federation", true, 31, 6 },
};
#define N3DS_LOG_CATEGORY_COUNT ((int32_t) (sizeof(kCategories) / sizeof(kCategories[0])))

typedef struct {
    Runner* runner;
    Renderer* renderer;
    int32_t scriptText;   // get_text
    int32_t font;         // the body font
    int32_t selected;     // the entry the list is on, as an index into kEntries
    int32_t scroll;       // how many pixels the list is scrolled past its start
    bool opened;          // showing one entry, rather than the list
    bool ready;
    char* title;          // cached page title
    char* text;           // cached entry text
    int32_t cachedEntry;  // which entry `text` belongs to, -1 for none
} N3DSLogScreen;

static N3DSLogScreen gLog;
// Picture backgrounds, one per entry index; -1 when the game does not have that one.
static int32_t gLogPicture[46];

void N3DSLogScreen_init(Runner* runner, Renderer* renderer) {
    free(gLog.title);
    free(gLog.text);
    memset(&gLog, 0, sizeof(gLog));
    gLog.runner = runner;
    gLog.renderer = renderer;
    gLog.cachedEntry = -1;
    gLog.scriptText = N3DSGml_scriptIndex("get_text");
    // The fonts in data.win carry a "Default" suffix; the bare name is a fallback.
    gLog.font = N3DSGml_fontIndex("fontGUI2Default");
    if (gLog.font < 0) gLog.font = N3DSGml_fontIndex("fontGUI2");
    // Each entry's picture is a background named after its index: bgLogImg00, bgLogImg01, ... Some have an A/B
    // variant; the plain name is used when there is one.
    for (int32_t i = 0; i < 46; i++) {
        char name[32];
        snprintf(name, sizeof(name), "bgLogImg%02d", (int) i);
        int32_t found = N3DSGml_backgroundIndex(name);
        if (found < 0) {
            // The variants the game ships (bgLogImg04B, bgLogImg26A, ...) stand in when there is no plain one.
            snprintf(name, sizeof(name), "bgLogImg%02dA", (int) i);
            found = N3DSGml_backgroundIndex(name);
        }
        if (found < 0) {
            snprintf(name, sizeof(name), "bgLogImg%02dB", (int) i);
            found = N3DSGml_backgroundIndex(name);
        }
        gLogPicture[i] = found;
    }
    gLog.ready = gLog.font >= 0;
    if (!gLog.ready) logInfo("Log screen: no fontGUI2Default, the panel is off\n");
}

bool N3DSLogScreen_available(void) {
    return gLog.ready;
}

// One string out of the language file, or NULL. The caller frees it.
static char* N3DSLogScreen_getText(const char* section, const char* key) {
    if (gLog.scriptText < 0) return NULL;
    RValue args[2] = { RValue_makeString(key), RValue_makeString(section) };
    RValue result = N3DSGml_callScript(gLog.scriptText, args, 2);
    char* text = result.type == RVALUE_STRING && result.string != NULL ? safeStrdup(result.string) : NULL;
    RValue_free(&result);
    return text;
}

// Whether the game says an entry has been found. global.log[] holds 0 until the entry is picked up.
static bool N3DSLogScreen_found(const N3DSLogEntry* entry) {
    double value = 0.0;
    if (!N3DSGml_getGlobal("log", entry->index, &value)) return false;
    return value != 0.0;
}

static void N3DSLogScreen_text1(const char* text, float x, float y, float scale, uint32_t colour) {
    if (text == NULL || text[0] == '\0') return;
    Renderer* r = gLog.renderer;
    r->drawFont = gLog.font;
    r->drawHalign = 0;
    r->drawAlpha = 1.0f;
    r->drawColor = 0x000000;
    r->vtable->drawText(r, text, x + 1.0f, y + 1.0f, scale, scale, 0.0f, -1.0f);
    r->drawColor = colour;
    r->vtable->drawText(r, text, x, y, scale, scale, 0.0f, -1.0f);
}

static void N3DSLogScreen_rect(float x0, float y0, float x1, float y1, uint32_t colour) {
    gLog.renderer->vtable->drawRectangle(gLog.renderer, x0, y0, x1, y1, colour, 1.0f, false);
}

// The entry's picture on the right of the list, filling the column beside it.
static void N3DSLogScreen_drawPicture(const N3DSLogEntry* entry, float x, float y, float w, float h) {
    if (entry->index < 0 || entry->index >= 46) return;
    int32_t background = gLogPicture[(int) entry->index];
    if (background < 0) return;
    int32_t tpag = Renderer_resolveBackgroundTPAGIndex(gLog.renderer->dataWin, background);
    if (tpag < 0) return;
    // The picture keeps its own proportion: the game's page shows it whole, not stretched.
    DataWin* dataWin = gLog.renderer->dataWin;
    if (dataWin == NULL || (uint32_t) tpag >= dataWin->tpag.count) return;
    float pw = (float) dataWin->tpag.items[tpag].boundingWidth;
    float ph = (float) dataWin->tpag.items[tpag].boundingHeight;
    if (pw <= 0.0f || ph <= 0.0f) return;
    float scale = w / pw;
    if (h / ph < scale) scale = h / ph;
    gLog.renderer->vtable->drawSprite(gLog.renderer, tpag, x, y, 0.0f, 0.0f, scale, scale, 0.0f, 0xFFFFFF, 1.0f);
}

// The list page: categories down the left, each category's entries under it, the selected entry's picture on the
// right.
static void N3DSLogScreen_drawList(void) {
    float y = (float) N3DS_LOG_BODY_Y - (float) gLog.scroll;
    for (int32_t c = 0; c < N3DS_LOG_CATEGORY_COUNT; c++) {
        const N3DSLogCategory* category = &kCategories[c];
        // The category heading, with its bar behind it, as the game's own list has it.
        N3DSLogScreen_rect(8.0f, y, (float) N3DS_LOG_LIST_W, y + 11.0f, N3DS_LOG_BAR_COLOUR);
        char* heading = N3DSLogScreen_getText(category->federation ? "NewLogsCategories" : "LogsCategories",
            category->heading);
        N3DSLogScreen_text1(heading != NULL ? heading : category->heading, 10.0f, y, 1.0f, 0xFFFFFF);
        free(heading);
        y += (float) N3DS_LOG_CAT_H;

        for (int32_t i = 0; i < category->count; i++) {
            int32_t entryIndex = category->first + i;
            const N3DSLogEntry* entry = &kEntries[entryIndex];
            if (y > (float) N3DS_LOG_FOOT_Y) break;
            bool found = N3DSLogScreen_found(entry);
            if (gLog.selected == entryIndex) {
                N3DSLogScreen_rect(8.0f, y - 2.0f, (float) N3DS_LOG_LIST_W, y + (float) N3DS_LOG_ROW_H - 4.0f,
                    N3DS_LOG_SELECT_COLOUR);
            }
            // A lock bullet for entries still missing, the way the game's list marks them; the title itself only
            // once it has been found.
            N3DSLogScreen_text1(found ? "!" : ".", 10.0f, y, 1.0f,
                found ? 0xFFFFFFu : N3DS_LOG_LOCKED_COLOUR);
            if (found) {
                char* title = N3DSLogScreen_getText(entry->federation ? "NewLogs" : "Logs", entry->key);
                N3DSLogScreen_text1(title != NULL ? title : entry->key, N3DS_LOG_TEXT_L, y, 1.0f, 0xFFFFFF);
                free(title);
            }
            y += (float) N3DS_LOG_ROW_H;
        }
        y += 4.0f; // a gap between categories
    }
    // The selected entry's picture fills the right-hand column while the list is up.
    const N3DSLogEntry* selected = &kEntries[gLog.selected];
    N3DSLogScreen_drawPicture(selected, (float) N3DS_LOG_PIC_X + 4.0f, (float) N3DS_LOG_BODY_Y,
        (float) (N3DS_LOG_W - N3DS_LOG_PIC_X - 8), (float) (N3DS_LOG_FOOT_Y - N3DS_LOG_BODY_Y));
}

// The entry page: its picture on the left, its text wrapped on the right.
static void N3DSLogScreen_drawEntry(void) {
    const N3DSLogEntry* entry = &kEntries[gLog.selected];
    N3DSLogScreen_drawPicture(entry, 8.0f, (float) N3DS_LOG_BODY_Y,
        (float) (N3DS_LOG_PIC_X - 16), (float) (N3DS_LOG_FOOT_Y - N3DS_LOG_BODY_Y));
    if (gLog.cachedEntry != gLog.selected) {
        free(gLog.text);
        gLog.text = N3DSLogScreen_getText(entry->federation ? "NewLogs" : "Logs", entry->key);
        gLog.cachedEntry = gLog.selected;
    }
    if (gLog.text == NULL) return;
    // The game marks its line breaks with '#'. The body is wrapped by hand: the port's own text drawing does not wrap,
    // and a log entry is far wider than the column.
    const float wrapWidth = (float) (N3DS_LOG_W - N3DS_LOG_PIC_X - 8);
    float x = (float) N3DS_LOG_PIC_X;
    float y = (float) N3DS_LOG_BODY_Y + 4.0f;
    char line[128];
    int32_t used = 0;
    for (const char* p = gLog.text; *p != '\0'; p++) {
        if (*p == '#') {
            line[used] = '\0';
            N3DSLogScreen_text1(line, x, y, 1.0f, 0xFFFFFF);
            used = 0;
            y += (float) N3DS_LOG_DESC_LINE;
            if (y > (float) N3DS_LOG_FOOT_Y) return;
            continue;
        }
        // Two bytes per Chinese character in the game's font at this size; count a wide character as two columns.
        int32_t width = ((unsigned char) *p) >= 0x80 ? 2 : 1;
        if ((float) (used + width) * 6.0f > wrapWidth) {
            line[used] = '\0';
            N3DSLogScreen_text1(line, x, y, 1.0f, 0xFFFFFF);
            used = 0;
            y += (float) N3DS_LOG_DESC_LINE;
            if (y > (float) N3DS_LOG_FOOT_Y) return;
        }
        line[used++] = *p;
        if (used >= (int32_t) sizeof(line) - 2) used = (int32_t) sizeof(line) - 2;
    }
    if (used > 0) {
        line[used] = '\0';
        N3DSLogScreen_text1(line, x, y, 1.0f, 0xFFFFFF);
    }
}

void N3DSLogScreen_draw(void) {
    if (!gLog.ready) return;
    Renderer* r = gLog.renderer;
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;

    N3DSLogScreen_rect(0.0f, 0.0f, (float) N3DS_LOG_W, (float) N3DS_LOG_H, 0x000000u);
    // The title strip, with the page's own name.
    N3DSLogScreen_rect(0.0f, (float) N3DS_LOG_TITLE_Y, (float) N3DS_LOG_W,
        (float) (N3DS_LOG_TITLE_Y + N3DS_LOG_TITLE_H), N3DS_LOG_BAR_COLOUR);
    char* title = N3DSLogScreen_getText("Subscreen", gLog.opened ? "Title_Logs" : "Title_Logs");
    if (title == NULL) title = safeStrdup("Logs");
    N3DSLogScreen_text1(title, (float) (N3DS_LOG_W / 2) - (float) (strlen(title) * 3), (float) N3DS_LOG_TITLE_Y + 1.0f,
        1.0f, 0xFFFFFF);
    free(title);

    if (gLog.opened) N3DSLogScreen_drawEntry();
    else N3DSLogScreen_drawList();

    // The footer strip and the button hints under it.
    N3DSLogScreen_rect(0.0f, (float) N3DS_LOG_FOOT_Y, (float) N3DS_LOG_W,
        (float) (N3DS_LOG_FOOT_Y + N3DS_LOG_FOOT_H), N3DS_LOG_BAR_COLOUR);
    N3DSLiveMap_drawButtons(2); // right slot: the log page

    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}

// Which entry a point in the list lands on, if any.
static int32_t N3DSLogScreen_entryAt(int32_t x, int32_t y) {
    if (x < 8 || x >= N3DS_LOG_LIST_W) return -1;
    float rowY = (float) N3DS_LOG_BODY_Y - (float) gLog.scroll;
    for (int32_t c = 0; c < N3DS_LOG_CATEGORY_COUNT; c++) {
        const N3DSLogCategory* category = &kCategories[c];
        rowY += (float) N3DS_LOG_CAT_H;
        for (int32_t i = 0; i < category->count; i++) {
            int32_t entryIndex = category->first + i;
            if ((float) y >= rowY - 2.0f && (float) y < rowY + (float) N3DS_LOG_ROW_H - 2.0f) return entryIndex;
            rowY += (float) N3DS_LOG_ROW_H;
        }
        rowY += 4.0f;
    }
    return -1;
}

void N3DSLogScreen_tap(int32_t x, int32_t y) {
    if (gLog.opened) {
        // Anywhere on an opened entry goes back to the list.
        gLog.opened = false;
        logInfo("Log screen: back to the list\n");
        return;
    }
    int32_t entry = N3DSLogScreen_entryAt(x, y);
    if (entry < 0) return;
    if (gLog.selected == entry && N3DSLogScreen_found(&kEntries[entry])) {
        gLog.opened = true;
        logInfo("Log screen: opened %s\n", kEntries[entry].key);
        return;
    }
    gLog.selected = entry;
}

void N3DSLogScreen_drag(int32_t dx, int32_t dy) {
    if (gLog.opened) return;
    gLog.scroll -= dy;
    if (gLog.scroll < 0) gLog.scroll = 0;
    // The whole list is taller than the column; stop it at its end.
    int32_t total = 0;
    for (int32_t c = 0; c < N3DS_LOG_CATEGORY_COUNT; c++) {
        total += N3DS_LOG_CAT_H + kCategories[c].count * N3DS_LOG_ROW_H + 4;
    }
    int32_t visible = N3DS_LOG_FOOT_Y - N3DS_LOG_BODY_Y;
    int32_t maxScroll = total - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (gLog.scroll > maxScroll) gLog.scroll = maxScroll;
    (void) dx;
}

int32_t N3DSLogScreen_foundCount(void) {
    int32_t count = 0;
    for (int32_t i = 0; i < N3DS_LOG_ENTRY_COUNT; i++) {
        if (N3DSLogScreen_found(&kEntries[i])) count++;
    }
    return count;
}
