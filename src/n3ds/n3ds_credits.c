#include "n3ds_credits.h"

#include "n3ds_gml.h"
#include "n3ds_platform_config.h"
#include "n3ds_renderer.h"

#include "../log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N3DS_CREDITS_W 320
#define N3DS_CREDITS_H 240

static struct {
    Runner* runner;
    Renderer* renderer;
    int32_t room;  // rm_credits, -1 when the game has no such room
    int32_t font;  // fontGUIDefault: the largest of the game's fonts, and the one the HUD text uses
    char author[192];
    char author2[192];
    char language[64];
} gCredits;

// Reads "key = "value"" from an ini's [section]. The port has no ini reader of its own for SD files beyond the one in
// n3ds_am2r.c, and these two keys are all this module needs.
static void N3DS_Credits_readString(const char* path, const char* section, const char* key, char* out, size_t outSize) {
    out[0] = '\0';
    FILE* f = fopen(path, "r");
    if (f == NULL) return;
    char line[512];
    bool inSection = false;
    while (fgets(line, sizeof(line), f) != NULL) {
        char* p = line;
        if (strlen(p) >= 3 && (unsigned char) p[0] == 0xEF && (unsigned char) p[1] == 0xBB && (unsigned char) p[2] == 0xBF) {
            p += 3; // UTF-8 BOM, which the language files start with
        }
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '[') {
            char name[64];
            size_t n = 0;
            p++;
            while (*p != ']' && *p != '\0' && n + 1 < sizeof(name)) name[n++] = *p++;
            name[n] = '\0';
            inSection = strcmp(name, section) == 0;
            continue;
        }
        if (!inSection) continue;
        size_t keyLen = strlen(key);
        if (strncmp(p, key, keyLen) != 0) continue;
        p += keyLen;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') continue;
        p++;
        while (*p == ' ' || *p == '\t') p++;
        char* value = p;
        size_t len = strlen(value);
        while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r' || value[len - 1] == ' ')) value[--len] = '\0';
        if (len >= 2 && value[0] == '"' && value[len - 1] == '"') {
            value[len - 1] = '\0';
            value++;
        }
        snprintf(out, outSize, "%s", value);
        break;
    }
    fclose(f);
}

// The language file in effect: config.ini's [Screen] Language is the game's own setting, and global.currentlanguage is
// what it has actually applied, so the global wins while the game runs.
static void N3DS_Credits_languageFile(char* out, size_t outSize) {
    out[0] = '\0';
    int32_t slot = 0;
    double current = 0.0;
    if (N3DSGml_getGlobal("currentlanguage", -1, &current) && current >= 0.0) slot = (int32_t) current;
    else {
        char text[32];
        N3DS_Credits_readString(N3DS_SD_DIR "config.ini", "Screen", "Language", text, sizeof(text));
        slot = (int32_t) strtol(text, NULL, 10);
    }
    FILE* f = fopen(N3DS_SD_DIR "lang/languages.txt", "r");
    if (f == NULL) return;
    char line[256];
    int32_t index = 0;
    while (fgets(line, sizeof(line), f) != NULL) {
        char* name = line;
        while (*name == ' ' || *name == '\t') name++;
        size_t len = strlen(name);
        while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r')) name[--len] = '\0';
        if (len == 0) continue;
        if (index == slot) {
            snprintf(out, outSize, "%s", name);
            break;
        }
        index++;
    }
    fclose(f);
}

void N3DSCredits_init(Runner* runner, Renderer* renderer) {
    memset(&gCredits, 0, sizeof(gCredits));
    gCredits.runner = runner;
    gCredits.renderer = renderer;
    gCredits.room = N3DSGml_roomIndex("rm_credits");
    gCredits.font = N3DSGml_fontIndex("fontGUIDefault");
    if (gCredits.font < 0) gCredits.font = N3DSGml_fontIndex("fontGUI2Default");
    char file[128];
    N3DS_Credits_languageFile(file, sizeof(file));
    if (file[0] == '\0') return;
    char path[256];
    snprintf(path, sizeof(path), N3DS_SD_DIR "lang/%s", file);
    N3DS_Credits_readString(path, "Header", "Author", gCredits.author, sizeof(gCredits.author));
    N3DS_Credits_readString(path, "Header", "Author2", gCredits.author2, sizeof(gCredits.author2));
    N3DS_Credits_readString(path, "Header", "Language", gCredits.language, sizeof(gCredits.language));
    logInfo("AM2R: credits %s: %s / %s\n", gCredits.language, gCredits.author, gCredits.author2);
}

// Centred line with the drop shadow the game's own small text uses.
static void N3DS_Credits_line(Renderer* r, const char* text, float y) {
    if (text == NULL || text[0] == '\0') return;
    r->drawColor = 0x000000;
    r->vtable->drawText(r, text, N3DS_CREDITS_W * 0.5f + 1.0f, y + 1.0f, 1.0f, 1.0f, 0.0f, -1.0f);
    r->drawColor = 0xFFFFFF;
    r->vtable->drawText(r, text, N3DS_CREDITS_W * 0.5f, y, 1.0f, 1.0f, 0.0f, -1.0f);
}

void N3DSCredits_draw(void) {
    if (gCredits.runner == NULL || gCredits.renderer == NULL || gCredits.room < 0) return;
    if (gCredits.runner->currentRoomIndex != gCredits.room) return;
    if (gCredits.author[0] == '\0' && gCredits.author2[0] == '\0') return;

    Renderer* r = gCredits.renderer;
    int32_t font = r->drawFont, halign = r->drawHalign;
    uint32_t color = r->drawColor;
    float alpha = r->drawAlpha;
    r->drawFont = gCredits.font >= 0 ? gCredits.font : font;
    r->drawHalign = 1; // the lines below are centred on N3DS_CREDITS_W / 2
    r->drawAlpha = 1.0f;

    N3DSRenderer_beginBottomScreenGUI(r, N3DS_CREDITS_W, N3DS_CREDITS_H);
    N3DS_Credits_line(r, "中文本地化", 70.0f);
    N3DS_Credits_line(r, gCredits.author, 100.0f);
    N3DS_Credits_line(r, gCredits.author2, 116.0f);
    N3DSRenderer_endBottomScreenGUI(r);

    r->drawFont = font;
    r->drawHalign = halign;
    r->drawColor = color;
    r->drawAlpha = alpha;
}
