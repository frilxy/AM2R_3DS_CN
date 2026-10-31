#include "n3ds_am2r.h"
#include "n3ds_am2r_native.h"

#include "n3ds_cjk_font.h"
#include "n3ds_gml.h"
#include "n3ds_pause.h"
#include "n3ds_platform_config.h"
#include "n3ds_renderer.h"
#include "n3ds_screen.h"

#include "../ini.h"
#include "../log.h"
#include "../renderer.h"
#include "../utils.h"
#include "../vm.h"

#include <stb/ds/stb_ds.h>
#include "stb_image.h"
#include <stdio.h>
#include <string.h>

// What this works with in AM2R (1.1 and the Community Updates):
//  - get_text(section, key) reads lang/<language>.ini; get_xjoybtnname(button) / get_xjoybtnsprite(button) give the
//    name ("A", "LB", "Back"...) and the icon (sXJoyBtnA...) of an XInput button for prompts and the Joypad menu.
//  - Options pages are objects created at (50, 92): oOptionsMain (rows Display, Sound, Control, [Extras,] Exit),
//    oOptionsDisplay, oOptionsControl... A page keeps its rows in op[] (oPauseOption / oOptionLR / oMenuLabel
//    instances with label, optext, optionid, enabled), skips rows with canedit[i] == 0, highlights the row whose
//    optionid is global.curropt and shows global.tiptext. Menu input comes from oControl.kUp/kDown/kLeft/kRight/
//    kMenu1/kMenu2 and k*PushedSteps (0 on the frame of the press). Going back recreates oOptionsMain.
//  - Weapons hurt enemies by their `damage` (oBeam, oMissile, oMissileExpl, oBombExpl, oPBombExpl); Metroids take a
//    fixed amount per missile instead, from myhealth.

#define N3DS_AM2R_PAGE_X 50.0f
#define N3DS_AM2R_PAGE_Y 92.0f
#define N3DS_AM2R_ROW_SEP 16.0f
#define N3DS_AM2R_SUPER_DAMAGE 5000.0
#define N3DS_AM2R_CHEATS_FILE N3DS_SD_DIR "cheats.ini"

enum { CHEAT_MASTER, CHEAT_HEALTH, CHEAT_AMMO, CHEAT_BEAM, CHEAT_BOMBS, CHEAT_MISSILES, CHEAT_POWERUPS, CHEAT_COUNT };
// global.item[0..N3DS_AM2R_ITEMS - 1]: the suits, balls, boots and beams (0 = not collected, 1 = have it on).
#define N3DS_AM2R_ITEMS 15
// The Cheats page's row after the switches opens the Teleport page: an area, a room in it, go (each page ends with Exit).
enum { CHEATS_PAGE_TELEPORT = CHEAT_COUNT, CHEATS_PAGE_ROWS };
enum { TELE_AREA, TELE_ROOM, TELE_GO, TELE_PAGE_ROWS };
#define N3DS_AM2R_AREAS 10
#define N3DS_AM2R_MAX_AREA_ROOMS 64
enum { DISPLAY_ROW_FRAMESKIP, DISPLAY_ROW_NATIVE, DISPLAY_ROW_FPS, DISPLAY_ROW_LIGHTING, DISPLAY_ROW_EXIT, DISPLAY_ROW_COUNT };
// The Extras page wraps the game's own Extras page (EXTRAS_ROW_GAME) and holds Cheats.
enum { EXTRAS_ROW_GAME, EXTRAS_ROW_CHEATS, EXTRAS_PAGE_ROWS };
// Rows a page shows at once (the tip bar covers the rest of the screen); longer pages scroll.
#define N3DS_AM2R_PAGE_VISIBLE_ROWS 6

typedef enum { PAGE_NONE, PAGE_DISPLAY, PAGE_EXTRAS, PAGE_CHEATS, PAGE_TELEPORT } N3DSAm2rPage;

static const char* const kCheatNames[CHEAT_COUNT] = { "master", "health", "ammo", "beam", "bombs", "missiles", "powerups" };

// The front end's own strings, defined with the language block below (it decides which of the two is drawn).
static const char* N3DSAm2r_text(const char* english, const char* chinese);

static const char* N3DSAm2r_cheatLabel(int32_t cheat) {
    switch (cheat) {
        case CHEAT_MASTER: return N3DSAm2r_text("Cheats", "金手指");
        case CHEAT_HEALTH: return N3DSAm2r_text("Unlimited Health", "无限生命");
        case CHEAT_AMMO: return N3DSAm2r_text("Unlimited Ammo", "无限弹药");
        case CHEAT_BEAM: return N3DSAm2r_text("Super Beam", "强化光束");
        case CHEAT_BOMBS: return N3DSAm2r_text("Super Bombs", "强化炸弹");
        case CHEAT_MISSILES: return N3DSAm2r_text("Super Missiles", "强化导弹");
        case CHEAT_POWERUPS: return N3DSAm2r_text("All Power-ups", "全能力");
        default: return "";
    }
}

static const char* N3DSAm2r_cheatTip(int32_t cheat) {
    switch (cheat) {
        case CHEAT_MASTER: return N3DSAm2r_text("Turn all cheats on or off", "打开或关闭全部金手指");
        case CHEAT_HEALTH: return N3DSAm2r_text("Energy stays full", "能量始终保持全满");
        case CHEAT_AMMO: return N3DSAm2r_text("Missiles, Super Missiles and Power Bombs stay full",
            "导弹、超级导弹与强力炸弹始终保持全满");
        case CHEAT_BEAM: return N3DSAm2r_text("Beam shots destroy anything they hit", "光束击中任何目标都会将其摧毁");
        case CHEAT_BOMBS: return N3DSAm2r_text("Bombs and Power Bombs destroy anything they hit",
            "炸弹与强力炸弹击中任何目标都会将其摧毁");
        case CHEAT_MISSILES: return N3DSAm2r_text("Missiles destroy anything they hit, Metroids too",
            "导弹击中任何目标都会将其摧毁，对 Metroid 也有效");
        case CHEAT_POWERUPS: return N3DSAm2r_text("Every suit, ball, boot and beam upgrade; back as it was when switched off",
            "所有服装、变形球、靴子与光束升级；关闭时恢复原状");
        default: return "";
    }
}

typedef struct {
    const char* name;
    BuiltinFunc hook;
    int32_t script; // the game's own script, called by the hook
} N3DSAm2rHook;

typedef struct {
    Runner* runner;
    bool active;
    int32_t objOptionsMain, objOptionsDisplay, objOptionsExtras, objOptionsControl, objControl;
    int32_t objMenuLabel, objOptionLR, objPauseOption, objWaterFX;
    int32_t objBeam, objMissile, objMissileExpl, objBombExpl, objPBombExpl;
    int32_t objMetroids[4];
    int32_t objCharacter;
    int32_t objSSFg, objSSBg, drawCoolText;
    int32_t sndSpin[5]; // the spin-jump loops: plain, Space Jump, Screw Attack, both, underwater
    int32_t sprA, sprB, sprX, sprY;
    int32_t sfxPlay, sndMenuMove, sndMenuSel;
    int32_t roomController, roomTitle;
    char* keyboardLabel; // what get_text gave for the Keyboard settings row
    int32_t lastControlPage;
    bool cheats[CHEAT_COUNT];
    // The Display or Cheats page while it is up: instance ids of its title and rows (the last one Exit).
    N3DSAm2rPage page;
    int32_t pageFrames;
    int32_t pageTitle;
    int32_t pageRows[CHEATS_PAGE_ROWS + 1 > DISPLAY_ROW_COUNT ? CHEATS_PAGE_ROWS + 1 : DISPLAY_ROW_COUNT];
    // Teleport: the game's rooms named rm_a<area digit>..., by area, in room order.
    int32_t teleRooms[N3DS_AM2R_AREAS][N3DS_AM2R_MAX_AREA_ROOMS];
    int32_t teleRoomCount[N3DS_AM2R_AREAS];
    int32_t teleArea, teleRoom;
    int32_t pageFirst; // the first row shown on a page that scrolls
    bool gameExtras;   // the game's own Extras page is up (not replaced by ours)
    // After a teleport: the room we are going to, and where Samus can stand there (checked in this order).
    int32_t teleTarget, teleFrames;
    int32_t objGotoRoom;
    int32_t objSpots[8];
    int32_t objSpotCount;
    // All Power-ups: the item states as they were, and which ones the cheat turned on.
    bool itemsForced;
    double itemBackup[N3DS_AM2R_ITEMS];
    bool itemChanged[N3DS_AM2R_ITEMS];
    bool pauseWidescreenSaved;
    double pauseWidescreen, pauseWidescreenSpace;
    bool gameRunning; // a game has been played since the title screen (global.ingame is 0 while paused)
} N3DSAm2r;

static N3DSAm2r gAm2r;

// get_text results by "<language>|<section>|<key>": get_text opens, reads and closes lang/<language>.ini on every call,
// and ini_close serializes the whole file (30 KB in English) each time, which shows as hitches when a room creates many
// labelled things. The language files aren't written while the game runs.
typedef struct {
    char* key;
    char* value;
} N3DSAm2rText;
static N3DSAm2rText* gTextCache = NULL;

static RValue hookGetText(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookGetXJoyBtnName(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookGetXJoyBtnSprite(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDamageSamus(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDamageSamusKnockdown(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDamageSamusPush(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookRoomChange(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookFontReplace(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDrawText(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookBackgroundReplace(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookSpriteReplace(VMContext* ctx, RValue* args, int32_t argCount);
// The builtin the hook replaced, so every other draw_text still runs the game's own.
static BuiltinFunc gDrawTextBuiltin = NULL;

static N3DSAm2rHook gHooks[] = {
    { "get_text", hookGetText, -1 },
    { "get_xjoybtnname", hookGetXJoyBtnName, -1 },
    { "get_xjoybtnsprite", hookGetXJoyBtnSprite, -1 },
    { "damage_samus", hookDamageSamus, -1 },
    { "damage_samus_knockdown", hookDamageSamusKnockdown, -1 },
    { "damage_samus_push", hookDamageSamusPush, -1 },
    { "room_change", hookRoomChange, -1 },
};

static int32_t N3DSAm2r_hookScript(BuiltinFunc hook) {
    for (size_t i = 0; i < sizeof(gHooks) / sizeof(gHooks[0]); i++) {
        if (gHooks[i].hook == hook) return gHooks[i].script;
    }
    return -1;
}

static RValue N3DSAm2r_callOriginal(BuiltinFunc hook, RValue* args, int32_t argCount) {
    return N3DSGml_callScript(N3DSAm2r_hookScript(hook), args, argCount);
}

// ===[ Nintendo buttons ]===

// "XBox 360 Joypad Configuration" -> "Nintendo 3DS Configuration".
static char* N3DSAm2r_nintendoText(const char* text) {
    static const char* const xbox[] = { "XBox 360 Joypad", "Xbox 360 Joypad", "XBOX 360 Joypad", "XBox 360", "Xbox 360", "XBOX 360" };
    for (size_t i = 0; i < sizeof(xbox) / sizeof(xbox[0]); i++) {
        const char* at = strstr(text, xbox[i]);
        if (at == NULL) continue;
        const char* with = "Nintendo 3DS";
        size_t before = (size_t) (at - text), skip = strlen(xbox[i]);
        char* out = safeMalloc(strlen(text) - skip + strlen(with) + 1);
        memcpy(out, text, before);
        strcpy(out + before, with);
        strcat(out, at + skip);
        return out;
    }
    return NULL;
}

// ===[ Languages ]===
// lang/languages.txt holds one language file per line, and config.ini's [Screen] Language is the line number (what the
// game keeps in global.currentlanguage). The game reads both itself; the 3DS front end's own pages are in no language
// file, so they need to know which language is active to follow it.

#define N3DS_AM2R_LANGUAGES 16

typedef struct {
    char* file;
    char* name;
} N3DSAm2rLanguage;

static N3DSAm2rLanguage gLanguages[N3DS_AM2R_LANGUAGES];
static int32_t gLanguageCount = 0;

static char* N3DSAm2r_trimLine(char* line) {
    while (*line == ' ' || *line == '\t') line++;
    char* end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return line;
}

// One "Key=Value" out of an ini file's section. The language files start with a UTF-8 BOM, which ini.c skips.
static void N3DSAm2r_readIniString(const char* path, const char* section, const char* key, char* out, size_t outSize) {
    out[0] = '\0';
    FILE* f = fopen(path, "rb");
    if (f == NULL) return;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return;
    }
    rewind(f);
    char* text = (char*) safeMalloc((size_t) size + 1u);
    size_t got = fread(text, 1, (size_t) size, f);
    fclose(f);
    text[got] = '\0';
    IniFile* ini = Ini_parse(text);
    free(text);
    if (ini == NULL) return;
    const char* value = Ini_getString(ini, section, key);
    if (value != NULL) {
        // GameMaker writes # for a line break inside a string.
        size_t at = 0;
        for (const char* c = value; *c != '\0' && at + 1u < outSize; c++) out[at++] = (*c == '#') ? ' ' : *c;
        out[at] = '\0';
    }
    Ini_free(ini);
}

static void N3DSAm2r_loadLanguages(void) {
    for (int32_t i = 0; i < gLanguageCount; i++) {
        free(gLanguages[i].file);
        free(gLanguages[i].name);
    }
    gLanguageCount = 0;
    FILE* f = fopen(N3DS_SD_DIR "lang/languages.txt", "r");
    if (f == NULL) {
        logInfo("AM2R: no " N3DS_SD_DIR "lang/languages.txt\n");
        return;
    }
    char line[256];
    while (gLanguageCount < N3DS_AM2R_LANGUAGES && fgets(line, sizeof(line), f) != NULL) {
        char* file = N3DSAm2r_trimLine(line);
        if (*file == '\0') continue;
        char path[512];
        snprintf(path, sizeof(path), N3DS_SD_DIR "lang/%s", file);
        FILE* probe = fopen(path, "r");
        if (probe == NULL) continue; // the game skips a language whose file is not there, and reuses the index
        fclose(probe);
        char name[256];
        N3DSAm2r_readIniString(path, "Header", "Language", name, sizeof(name));
        gLanguages[gLanguageCount].file = safeStrdup(file);
        gLanguages[gLanguageCount].name = safeStrdup(name[0] != '\0' ? name : file);
        logInfo("AM2R: language %d: %s (%s)\n", (int) gLanguageCount, file, gLanguages[gLanguageCount].name);
        gLanguageCount++;
    }
    fclose(f);
}

// The selected language's index. config.ini's [Screen] Language is the game's setting; global.currentlanguage is what
// it has actually applied, so it wins once the game is running.
static int32_t N3DSAm2r_languageSlot(void) {
    double current = 0.0;
    if (N3DSGml_getGlobal("currentlanguage", -1, &current) && current >= 0.0) return (int32_t) current;
    char text[64];
    N3DSAm2r_readIniString(N3DS_SD_DIR "config.ini", "Screen", "Language", text, sizeof(text));
    return text[0] != '\0' ? (int32_t) strtol(text, NULL, 10) : 0;
}

// True while a language whose file starts with this prefix is selected.
static bool N3DSAm2r_languageIs(const char* prefix) {
    int32_t slot = N3DSAm2r_languageSlot();
    if (slot < 0 || slot >= gLanguageCount) return false;
    const char* file = gLanguages[slot].file;
    return file != NULL && strncmp(file, prefix, strlen(prefix)) == 0;
}

// The front end's own pages in English and Chinese. Only Chinese is translated (a wrong translation is worse than the
// English string), and the characters it uses are baked into the font pack by tools/n3ds/fontgen/make_cjk_font.py,
// which reads the Chinese literals out of this directory.
static const char* N3DSAm2r_text(const char* english, const char* chinese) {
    return N3DSAm2r_languageIs("schinese") ? chinese : english;
}

static RValue hookGetText(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    const char* section = argCount > 0 && args[0].type == RVALUE_STRING ? args[0].string : "";
    const char* key = argCount > 1 && args[1].type == RVALUE_STRING ? args[1].string : "";
    // The port replaces the game's Display page with its own, so the row's tip is the port's to write.
    if (strcmp(section, "OptionsMain") == 0 && strcmp(key, "Display_Tip") == 0) {
        return RValue_makeOwnedString(safeStrdup(N3DSAm2r_text("Display and performance settings", "画面与性能设置")));
    }
    char cacheKey[256] = "";
    if (argCount == 2 && args[0].type == RVALUE_STRING && args[1].type == RVALUE_STRING) {
        double language = 0.0;
        N3DSGml_getGlobal("currentlanguage", -1, &language);
        int n = snprintf(cacheKey, sizeof(cacheKey), "%g|%s|%s", language, section, key);
        if (n < 0 || n >= (int) sizeof(cacheKey)) cacheKey[0] = '\0';
    }
    if (cacheKey[0] != '\0') {
        // Before the first shgeti, which would make a map that doesn't copy its keys.
        if (gTextCache == NULL) sh_new_strdup(gTextCache);
        ptrdiff_t at = shgeti(gTextCache, cacheKey);
        if (at >= 0) return RValue_makeOwnedString(safeStrdup(gTextCache[at].value));
    }
    RValue result = N3DSAm2r_callOriginal(hookGetText, args, argCount);
    if (result.type != RVALUE_STRING || result.string == NULL) return result;
    if (strcmp(section, "OptionsControl") == 0 && strcmp(key, "KeyboardSettings") == 0) {
        free(gAm2r.keyboardLabel);
        gAm2r.keyboardLabel = safeStrdup(result.string);
    }
    char* nintendo = N3DSAm2r_nintendoText(result.string);
    if (nintendo != NULL) {
        RValue_free(&result);
        result = RValue_makeOwnedString(nintendo);
    }
    if (cacheKey[0] != '\0') {
        shput(gTextCache, cacheKey, safeStrdup(result.string));
    }
    return result;
}

// Xbox buttons by position -> the 3DS buttons in those positions.
static RValue hookGetXJoyBtnName(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    static const char* const map[][2] = {
        { "A", "B" }, { "B", "A" }, { "X", "Y" }, { "Y", "X" }, { "LB", "L" }, { "RB", "R" }, { "LT", "ZL" },
        { "RT", "ZR" }, { "LStick", "Circle Pad" }, { "RStick", "C-Stick" }, { "Back", "Select" },
    };
    RValue result = N3DSAm2r_callOriginal(hookGetXJoyBtnName, args, argCount);
    if (result.type != RVALUE_STRING || result.string == NULL) return result;
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(result.string, map[i][0]) != 0) continue;
        RValue_free(&result);
        return RValue_makeOwnedString(safeStrdup(map[i][1]));
    }
    return result;
}

// The icons show Xbox letters: the 3DS's B is where the Xbox's A is (and so on), so swap A/B and X/Y.
static RValue hookGetXJoyBtnSprite(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    RValue result = N3DSAm2r_callOriginal(hookGetXJoyBtnSprite, args, argCount);
    int32_t sprite = RValue_toInt32(result);
    int32_t swapped = sprite;
    if (sprite == gAm2r.sprA) swapped = gAm2r.sprB;
    else if (sprite == gAm2r.sprB) swapped = gAm2r.sprA;
    else if (sprite == gAm2r.sprX) swapped = gAm2r.sprY;
    else if (sprite == gAm2r.sprY) swapped = gAm2r.sprX;
    if (swapped == sprite || swapped < 0) return result;
    RValue_free(&result);
    return RValue_makeReal((GMLReal) swapped);
}

// ===[ Boot ]===

// room_change(room, transition): at boot the game shows rm_controller (a "use a gamepad" notice) before the title;
// on the 3DS that's skipped, straight to the title screen (the notice's room only waits and goes there).
static RValue hookRoomChange(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount > 0 && gAm2r.roomController >= 0 && gAm2r.roomTitle >= 0 && RValue_toInt32(args[0]) == gAm2r.roomController) {
        RValue_free(&args[0]);
        args[0] = RValue_makeReal((GMLReal) gAm2r.roomTitle);
    }
    return N3DSAm2r_callOriginal(hookRoomChange, args, argCount);
}

// ===[ Language packs: font_replace ]===

// font_replace(font, ttf, size, bold, italic, first, last): the language packs swap whole fonts at runtime, because
// data.win itself carries no CJK glyphs. The game passes more arguments than GameMaker documents and only the first
// three matter here. No script implements it (the port is the only implementation), so it is installed by name below
// rather than through gHooks, which only wraps functions the game defines too.
//
// The glyphs come from the pack the offline generator baked (see n3ds_cjk_font.h): the TTF is never opened at runtime.
static RValue hookFontReplace(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 3 || args[1].type != RVALUE_STRING || args[1].string == NULL) return RValue_makeUndefined();
    if (gAm2r.runner == NULL) return RValue_makeUndefined();
    int32_t fontIndex = RValue_toInt32(args[0]);
    float emSize = (float) RValue_toReal(args[2]);
    if (!N3DSCjkFont_apply(gAm2r.runner, fontIndex, args[1].string, emSize)) {
        logInfo("AM2R: font_replace(%d, %s, %g) was not applied\n", (int) fontIndex, args[1].string, (double) emSize);
    }
    return RValue_makeUndefined();
}

// draw_text(x, y, string). One string the game draws is a literal in its own bytecode, not a language-file key, so no
// translation can reach it: the pause screen draws "[Select] -> Logs" in oSubScrTopLayer's Draw event. Only that exact
// string is translated -- everything else passes straight through, so the HUD, dialogue and item screens are untouched.
static RValue hookDrawText(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount == 3 && args[2].type == RVALUE_STRING && args[2].string != NULL &&
        strcmp(args[2].string, "[Select] -> Logs") == 0 && N3DSAm2r_languageIs("schinese")) {
        RValue translated = RValue_makeOwnedString(safeStrdup("[Select] -> 日志"));
        RValue a[3] = { args[0], args[1], translated };
        a[2].ownsReference = false;
        RValue result = gDrawTextBuiltin != NULL ? gDrawTextBuiltin(ctx, a, 3) : RValue_makeUndefined();
        RValue_free(&translated);
        return result;
    }
    return gDrawTextBuiltin != NULL ? gDrawTextBuiltin(ctx, args, argCount) : RValue_makeUndefined();
}

// background_replace(background, png, removeback, smooth, xorigin, yorigin). The language packs use it for the title
// logo: the game scans lang/titles/*.png, parses the file name for the language and the logo's offset, and swaps the
// title room's background for the picture. The PNG is decoded and uploaded whole; the background then draws at the
// image's real size, which is what makes the logo line up with the title screen.
//
// The path is game-relative ("lang/titles/<file>.png"), so it is read through the runner's FileSystem (romfs, then the
// SD folder) rather than fopen, which has no idea where the game lives.
static RValue hookBackgroundReplace(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 2 || args[1].type != RVALUE_STRING || args[1].string == NULL) return RValue_makeUndefined();
    Runner* runner = gAm2r.runner;
    DataWin* dw = runner != NULL ? runner->dataWin : NULL;
    if (dw == NULL || runner->fileSystem == NULL) return RValue_makeUndefined();
    int32_t backgroundIndex = RValue_toInt32(args[0]);
    if (backgroundIndex < 0 || (uint32_t) backgroundIndex >= dw->bgnd.count) return RValue_makeUndefined();

    uint8_t* png = NULL;
    int32_t pngSize = 0;
    bool read = runner->fileSystem->vtable->readFileBinary(runner->fileSystem, args[1].string, &png, &pngSize);
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = (read && png != NULL && pngSize > 0) ? stbi_load_from_memory(png, pngSize, &width, &height, &channels, 4) : NULL;
    free(png);
    if (pixels == NULL || width <= 0 || height <= 0) {
        logInfo("AM2R: background_replace could not read %s\n", args[1].string);
        if (pixels != NULL) stbi_image_free(pixels);
        return RValue_makeUndefined();
    }

    int32_t tpagIndex = N3DSRenderer_allocImagePageRGBA8(runner->renderer, pixels, (uint32_t) width, (uint32_t) height);
    stbi_image_free(pixels);
    if (tpagIndex < 0) {
        logError("AM2R: background_replace could not upload %s (%dx%d)\n", args[1].string, width, height);
        return RValue_makeUndefined();
    }
    // A background draws through its tpagIndex, and the item the page added is already sized to the image.
    dw->bgnd.backgrounds[backgroundIndex].tpagIndex = tpagIndex;
    logInfo("AM2R: background %d -> %s (%dx%d, tpag %d)\n", (int) backgroundIndex, args[1].string, width, height, (int) tpagIndex);
    return RValue_makeUndefined();
}

// sprite_replace(sprite, png, imgnum, removeback, smooth, xorig, yorig). The language packs use it for the area title
// banners: AM2R's own headers are English animation strips (sIntro_A1..A8, 19-31 frames of 10px text), and a pack
// ships its language's banner as ONE wide strip whose frame width is encoded in the file name
// (<language>_a<area>_f<frameWidth>_b0_c3_d<..>_e<..>.png). The game replaces the sprite's image with that strip and
// draws pieces of it with draw_sprite_part, so all this has to do is put the picture in and let the sprite be that
// wide; the frame width stays the game's business.
static RValue hookSpriteReplace(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 2 || args[1].type != RVALUE_STRING || args[1].string == NULL) return RValue_makeUndefined();
    Runner* runner = gAm2r.runner;
    DataWin* dw = runner != NULL ? runner->dataWin : NULL;
    if (dw == NULL || runner->fileSystem == NULL || runner->renderer == NULL) return RValue_makeUndefined();
    int32_t spriteIndex = RValue_toInt32(args[0]);
    if (spriteIndex < 0 || (uint32_t) spriteIndex >= dw->sprt.count) return RValue_makeUndefined();
    Sprite* sprite = &dw->sprt.sprites[spriteIndex];
    if (sprite->tpagIndices == NULL || sprite->textureCount == 0) return RValue_makeUndefined();
    int32_t imageIndex = argCount > 2 ? RValue_toInt32(args[2]) : 0;
    if (imageIndex < 0 || (uint32_t) imageIndex >= sprite->textureCount) imageIndex = 0;

    uint8_t* png = NULL;
    int32_t pngSize = 0;
    bool read = runner->fileSystem->vtable->readFileBinary(runner->fileSystem, args[1].string, &png, &pngSize);
    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = (read && png != NULL && pngSize > 0) ? stbi_load_from_memory(png, pngSize, &width, &height, &channels, 4) : NULL;
    free(png);
    if (pixels == NULL || width <= 0 || height <= 0) {
        logInfo("AM2R: sprite_replace could not read %s\n", args[1].string);
        if (pixels != NULL) stbi_image_free(pixels);
        return RValue_makeUndefined();
    }

    int32_t tpagIndex = N3DSRenderer_allocImagePageRGBA8(runner->renderer, pixels, (uint32_t) width, (uint32_t) height);
    stbi_image_free(pixels);
    if (tpagIndex < 0) {
        logError("AM2R: sprite_replace could not upload %s (%dx%d)\n", args[1].string, width, height);
        return RValue_makeUndefined();
    }

    // The image goes in at imgnum and the sprite becomes as large as the picture: the pack's strip is wider than the
    // English frames, and the game reads sprite_get_width to know how many pieces it has.
    sprite->tpagIndices[imageIndex] = tpagIndex;
    sprite->width = (uint32_t) width;
    sprite->height = (uint32_t) height;
    if (argCount > 6) {
        sprite->originX = (int32_t) RValue_toInt32(args[5]);
        sprite->originY = (int32_t) RValue_toInt32(args[6]);
    }
    logInfo("AM2R: sprite %d (%s) image %d -> %s (%dx%d, tpag %d)\n",
        (int) spriteIndex, sprite->name != NULL ? sprite->name : "?", (int) imageIndex, args[1].string, width, height, (int) tpagIndex);
    return RValue_makeUndefined();
}

// ===[ Unlimited health ]===

static bool N3DSAm2r_cheat(int which);

// damage_samus(damage, hpush, vpush, ...) and the knockdown / push variants take the damage first: none with the
// health cheat, the knockback stays.
static RValue N3DSAm2r_damage(BuiltinFunc hook, RValue* args, int32_t argCount) {
    if (argCount > 0 && N3DSAm2r_cheat(CHEAT_HEALTH)) {
        RValue_free(&args[0]);
        args[0] = RValue_makeReal(0.0);
    }
    return N3DSAm2r_callOriginal(hook, args, argCount);
}

static RValue hookDamageSamus(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    return N3DSAm2r_damage(hookDamageSamus, args, argCount);
}

static RValue hookDamageSamusKnockdown(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    return N3DSAm2r_damage(hookDamageSamusKnockdown, args, argCount);
}

static RValue hookDamageSamusPush(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    return N3DSAm2r_damage(hookDamageSamusPush, args, argCount);
}

// ===[ Cheats file ]===

static void N3DSAm2r_loadCheats(void) {
    FILE* f = fopen(N3DS_AM2R_CHEATS_FILE, "r");
    if (f == NULL) return;
    char line[64];
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[32];
        int value = 0;
        if (sscanf(line, "%31[^=]=%d", name, &value) != 2) continue;
        if (strcmp(name, "frameskip") == 0) N3DS_setFrameskip(value != 0);
        if (strcmp(name, "native") == 0) N3DSAm2rNative_setEnabled(value != 0);
        if (strcmp(name, "fps") == 0) N3DS_setFpsCounter(value != 0);
        if (strcmp(name, "lighting") == 0) N3DSAm2rNative_setLighting(value != 0);
        for (int i = 0; i < CHEAT_COUNT; i++) {
            if (strcmp(name, kCheatNames[i]) == 0) gAm2r.cheats[i] = value != 0;
        }
    }
    fclose(f);
}

static void N3DSAm2r_saveCheats(void) {
    FILE* f = fopen(N3DS_AM2R_CHEATS_FILE, "w");
    if (f == NULL) return;
    for (int i = 0; i < CHEAT_COUNT; i++) fprintf(f, "%s=%d\n", kCheatNames[i], gAm2r.cheats[i] ? 1 : 0);
    fprintf(f, "frameskip=%d\n", N3DS_getFrameskip() ? 1 : 0);
    fprintf(f, "native=%d\n", N3DSAm2rNative_enabled() ? 1 : 0);
    fprintf(f, "fps=%d\n", N3DS_getFpsCounter() ? 1 : 0);
    fprintf(f, "lighting=%d\n", N3DSAm2rNative_lighting() ? 1 : 0);
    fclose(f);
}

static bool N3DSAm2r_cheat(int which) {
    return gAm2r.cheats[CHEAT_MASTER] && gAm2r.cheats[which];
}

// ===[ Teleport ]===

static void N3DSAm2r_collectRooms(void) {
    DataWin* dataWin = gAm2r.runner->dataWin;
    for (uint32_t i = 0; i < dataWin->room.count; i++) {
        const char* name = dataWin->room.rooms[i].name;
        if (name == NULL || strncmp(name, "rm_a", 4) != 0 || name[4] < '0' || name[4] > '9') continue;
        int area = name[4] - '0';
        if (gAm2r.teleRoomCount[area] < N3DS_AM2R_MAX_AREA_ROOMS) gAm2r.teleRooms[area][gAm2r.teleRoomCount[area]++] = (int32_t) i;
    }
    while (gAm2r.teleArea < N3DS_AM2R_AREAS - 1 && gAm2r.teleRoomCount[gAm2r.teleArea] == 0) gAm2r.teleArea++;
}

static const char* N3DSAm2r_teleRoomName(void) {
    int32_t n = gAm2r.teleRoomCount[gAm2r.teleArea];
    if (n == 0) return "-";
    return gAm2r.runner->dataWin->room.rooms[gAm2r.teleRooms[gAm2r.teleArea][gAm2r.teleRoom % n]].name;
}

static void N3DSAm2r_teleStep(int row, int delta) {
    if (row == TELE_AREA) {
        int a = gAm2r.teleArea;
        for (int k = 0; k < N3DS_AM2R_AREAS; k++) {
            a = (a + N3DS_AM2R_AREAS + delta) % N3DS_AM2R_AREAS;
            if (gAm2r.teleRoomCount[a] > 0) break;
        }
        gAm2r.teleArea = a;
        gAm2r.teleRoom = 0;
    } else {
        int n = gAm2r.teleRoomCount[gAm2r.teleArea];
        if (n > 0) gAm2r.teleRoom = (gAm2r.teleRoom + n + delta) % n;
    }
}

// ===[ Init ]===

void N3DSAm2r_init(Runner* runner) {
    memset(&gAm2r, 0, sizeof(gAm2r));
    gAm2r.runner = runner;
    gAm2r.lastControlPage = -1;
    gAm2r.objOptionsMain = N3DSGml_objectIndex("oOptionsMain");
    gAm2r.objOptionsDisplay = N3DSGml_objectIndex("oOptionsDisplay");
    gAm2r.objOptionsExtras = N3DSGml_objectIndex("oOptionsExtras");
    gAm2r.teleTarget = -1;
    gAm2r.objGotoRoom = N3DSGml_objectIndex("oGotoRoom");
    {
        static const char* const kSpots[] = { "oSaveStation", "oDoor", "oDoorA4", "oDoorA5", "oDoorA8", "oA3LabDoor", "oElevator" };
        for (size_t i = 0; i < sizeof(kSpots) / sizeof(kSpots[0]); i++) {
            int32_t o = N3DSGml_objectIndex(kSpots[i]);
            if (o >= 0) gAm2r.objSpots[gAm2r.objSpotCount++] = o;
        }
    }
    gAm2r.objOptionsControl = N3DSGml_objectIndex("oOptionsControl");
    gAm2r.objControl = N3DSGml_objectIndex("oControl");
    gAm2r.objMenuLabel = N3DSGml_objectIndex("oMenuLabel");
    gAm2r.objOptionLR = N3DSGml_objectIndex("oOptionLR");
    gAm2r.objPauseOption = N3DSGml_objectIndex("oPauseOption");
    gAm2r.objWaterFX = N3DSGml_objectIndex("oWaterFXV2");
    gAm2r.objBeam = N3DSGml_objectIndex("oBeam");
    gAm2r.objMissile = N3DSGml_objectIndex("oMissile");
    gAm2r.objMissileExpl = N3DSGml_objectIndex("oMissileExpl");
    gAm2r.objBombExpl = N3DSGml_objectIndex("oBombExpl");
    gAm2r.objPBombExpl = N3DSGml_objectIndex("oPBombExpl");
    gAm2r.objMetroids[0] = N3DSGml_objectIndex("oMAlpha");
    gAm2r.objMetroids[1] = N3DSGml_objectIndex("oMGamma");
    gAm2r.objMetroids[2] = N3DSGml_objectIndex("oMZeta");
    gAm2r.objMetroids[3] = N3DSGml_objectIndex("oMOmega");
    gAm2r.sprA = N3DSGml_spriteIndex("sXJoyBtnA");
    gAm2r.sprB = N3DSGml_spriteIndex("sXJoyBtnB");
    gAm2r.sprX = N3DSGml_spriteIndex("sXJoyBtnX");
    gAm2r.sprY = N3DSGml_spriteIndex("sXJoyBtnY");
    gAm2r.sfxPlay = N3DSGml_scriptIndex("sfx_play");
    gAm2r.roomController = N3DSGml_roomIndex("rm_controller");
    gAm2r.roomTitle = N3DSGml_roomIndex("titleroom");
    gAm2r.sndMenuMove = N3DSGml_soundIndex("sndMenuMove");
    gAm2r.sndMenuSel = N3DSGml_soundIndex("sndMenuSel");
    gAm2r.objCharacter = N3DSGml_objectIndex("oCharacter");
    gAm2r.objSSFg = N3DSGml_objectIndex("oSS_Fg");
    gAm2r.objSSBg = N3DSGml_objectIndex("oSS_Bg");
    gAm2r.drawCoolText = N3DSGml_scriptIndex("draw_cool_text");
    {
        static const char* const kSpin[5] = { "sndSpinJump", "sndSpaceJump", "sndScrewAttack", "sndSpaceScrewAttack", "sndSpinJumpWater" };
        repeat(5, i) gAm2r.sndSpin[i] = N3DSGml_soundIndex(kSpin[i]);
    }
    gAm2r.active = gAm2r.objOptionsMain >= 0 && gAm2r.objControl >= 0;
    if (!gAm2r.active) return;
    N3DSAm2r_loadLanguages();

    VMContext* vm = runner->vmContext;
    DataWin* dataWin = runner->dataWin;
    for (uint32_t i = 0; i < vm->funcCallCacheCount; i++) {
        const char* name = dataWin->func.functions[i].name;
        if (name == NULL) continue;
        for (size_t h = 0; h < sizeof(gHooks) / sizeof(gHooks[0]); h++) {
            if (strcmp(name, gHooks[h].name) != 0) continue;
            int32_t script = N3DSGml_scriptIndex(name);
            if (script < 0) break;
            gHooks[h].script = script;
            vm->funcCallCache[i].builtin = (void*) gHooks[h].hook;
            logInfo("AM2R: %s goes through the 3DS front end\n", name);
        }
    }
    // font_replace has no game script behind it (the loop above only wraps functions the game also defines), so its
    // cached builtin is replaced by name. This has to happen before oControl's Create event, which is where the
    // language pack calls it.
    for (uint32_t i = 0; i < vm->funcCallCacheCount; i++) {
        const char* name = dataWin->func.functions[i].name;
        if (name == NULL) continue;
        if (strcmp(name, "font_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookFontReplace;
            logInfo("AM2R: font_replace goes through the 3DS front end\n");
        } else if (strcmp(name, "draw_text") == 0) {
            gDrawTextBuiltin = (BuiltinFunc) vm->funcCallCache[i].builtin;
            vm->funcCallCache[i].builtin = (void*) hookDrawText;
        } else if (strcmp(name, "background_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookBackgroundReplace;
            logInfo("AM2R: background_replace goes through the 3DS front end\n");
        } else if (strcmp(name, "sprite_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookSpriteReplace;
            logInfo("AM2R: sprite_replace goes through the 3DS front end\n");
        }
    }
    // Fast scripts default to on; cheats.ini can turn them off.
    N3DSAm2rNative_setEnabled(true);
    N3DSAm2r_loadCheats();
    N3DS_setScreenMode(N3DS_SCREEN_STRETCH); // the only screen mode: the others made no sense for this game
    N3DSAm2r_collectRooms();
    N3DSAm2rNative_init(runner);
}

// ===[ Options ]===

static void N3DSAm2r_sfx(int32_t sound) {
    if (gAm2r.sfxPlay < 0 || sound < 0) return;
    RValue arg = RValue_makeReal((GMLReal) sound);
    RValue result = N3DSGml_callScript(gAm2r.sfxPlay, &arg, 1);
    RValue_free(&result);
}

// The Keyboard settings row of the Control page: hidden, the rows below move up.
static void N3DSAm2r_hideKeyboardRow(void) {
    Instance* page = N3DSGml_firstInstance(gAm2r.objOptionsControl);
    if (page == NULL || gAm2r.keyboardLabel == NULL || (int32_t) page->instanceId == gAm2r.lastControlPage) return;
    gAm2r.lastControlPage = (int32_t) page->instanceId;
    double last = 0.0;
    if (!N3DSGml_getVar(page, "lastitem", &last)) return;
    for (int32_t k = 0; k <= (int32_t) last; k++) {
        double id = -1.0;
        if (!N3DSGml_getVarElement(page, "op", k, &id)) continue;
        Instance* row = N3DSGml_instanceById((int32_t) id);
        const char* label = row != NULL ? N3DSGml_getVarString(row, "label") : NULL;
        if (label == NULL || strcmp(label, gAm2r.keyboardLabel) != 0) continue;
        N3DSGml_setVarElement(page, "canedit", k, 0.0);
        row->visible = false;
        for (int32_t j = k + 1; j <= (int32_t) last; j++) {
            double otherId = -1.0;
            if (!N3DSGml_getVarElement(page, "op", j, &otherId)) continue;
            Instance* other = N3DSGml_instanceById((int32_t) otherId);
            if (other != NULL && other->y > row->y) other->y -= N3DS_AM2R_ROW_SEP;
        }
        double curropt = 0.0;
        if (N3DSGml_getGlobal("curropt", -1, &curropt) && (int32_t) curropt == k) N3DSGml_setGlobal("curropt", (double) (k + 1));
        return;
    }
}

static Instance* N3DSAm2r_pageRow(int i) {
    return N3DSGml_instanceById(gAm2r.pageRows[i]);
}

// Rows of each page (the last one is Exit).
static int N3DSAm2r_pageRowCount(N3DSAm2rPage page) {
    return page == PAGE_DISPLAY ? DISPLAY_ROW_COUNT : page == PAGE_EXTRAS ? EXTRAS_PAGE_ROWS + 1
        : page == PAGE_TELEPORT ? TELE_PAGE_ROWS + 1 : CHEATS_PAGE_ROWS + 1;
}

// Places the page's rows: the window of rows around the selection, the others hidden.
static void N3DSAm2r_layoutRows(int32_t selected) {
    int count = N3DSAm2r_pageRowCount(gAm2r.page);
    if (selected >= 0 && selected < count) {
        if (selected < gAm2r.pageFirst) gAm2r.pageFirst = selected;
        if (selected >= gAm2r.pageFirst + N3DS_AM2R_PAGE_VISIBLE_ROWS) gAm2r.pageFirst = selected - N3DS_AM2R_PAGE_VISIBLE_ROWS + 1;
    }
    if (count <= N3DS_AM2R_PAGE_VISIBLE_ROWS) gAm2r.pageFirst = 0;
    for (int i = 0; i < count; i++) {
        Instance* row = N3DSAm2r_pageRow(i);
        if (row == NULL) continue;
        row->y = N3DS_AM2R_PAGE_Y - 8.0f + N3DS_AM2R_ROW_SEP * (float) (i + 1 - gAm2r.pageFirst);
        row->visible = i >= gAm2r.pageFirst && i < gAm2r.pageFirst + N3DS_AM2R_PAGE_VISIBLE_ROWS;
    }
}

static void N3DSAm2r_refreshPage(void) {
    double curropt = 0.0;
    N3DSGml_getGlobal("curropt", -1, &curropt);
    int32_t selected = (int32_t) curropt;
    N3DSAm2r_layoutRows(selected);
    if (gAm2r.page == PAGE_EXTRAS) {
        N3DSGml_setGlobalString("tiptext", selected == EXTRAS_ROW_GAME ? N3DSAm2r_text("The game's own extras", "游戏自带的额外内容")
            : selected == EXTRAS_ROW_CHEATS ? N3DSAm2r_text("Unlimited health and ammo, stronger weapons, teleport",
                "无限生命和弹药、更强的武器、传送")
            : N3DSAm2r_text("Back to the options", "回到选项"));
        return;
    }
    if (gAm2r.page == PAGE_DISPLAY) {
        Instance* frameskip = N3DSAm2r_pageRow(DISPLAY_ROW_FRAMESKIP);
        if (frameskip != NULL) N3DSGml_setVarString(frameskip, "optext", N3DS_getFrameskip()
            ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        Instance* native = N3DSAm2r_pageRow(DISPLAY_ROW_NATIVE);
        if (native != NULL) {
            N3DSGml_setVarString(native, "optext", N3DSAm2rNative_available() == 0 ? N3DSAm2r_text("N/A", "不可用")
                : N3DSAm2rNative_enabled() ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        }
        Instance* fps = N3DSAm2r_pageRow(DISPLAY_ROW_FPS);
        if (fps != NULL) N3DSGml_setVarString(fps, "optext", N3DS_getFpsCounter()
            ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        Instance* lighting = N3DSAm2r_pageRow(DISPLAY_ROW_LIGHTING);
        if (lighting != NULL) N3DSGml_setVarString(lighting, "optext", N3DSAm2rNative_lighting()
            ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        const char* tip = selected == DISPLAY_ROW_FRAMESKIP ? N3DSAm2r_text(
                "A steady 30 fps at full game speed (draws every other frame)",
                "保持完整游戏速度下稳定的 30 帧（隔帧绘制）")
            : selected == DISPLAY_ROW_NATIVE ? (N3DSAm2rNative_available() == 0
                ? N3DSAm2r_text("Not available for this version of the game", "此版本的游戏不支持")
                : N3DSAm2r_text("Busy game scripts run as built-in code: faster, same behaviour",
                    "繁重的游戏脚本改用内置代码运行：更快，行为相同"))
            : selected == DISPLAY_ROW_FPS ? N3DSAm2r_text("A small frame rate counter in the bottom-left corner",
                "左下角的小型帧率计数器")
            : selected == DISPLAY_ROW_LIGHTING ? N3DSAm2r_text(
                "The dark rooms' light effect. OFF draws them fully lit, which is also faster",
                "暗房间的光照效果；关闭后全亮，也更快")
            : N3DSAm2r_text("Back to the options", "回到选项");
        N3DSGml_setGlobalString("tiptext", tip);
        return;
    }
    if (gAm2r.page == PAGE_TELEPORT) {
        char areaText[8];
        snprintf(areaText, sizeof(areaText), "A%d", (int) gAm2r.teleArea);
        Instance* area = N3DSAm2r_pageRow(TELE_AREA);
        if (area != NULL) N3DSGml_setVarString(area, "optext", areaText);
        Instance* room = N3DSAm2r_pageRow(TELE_ROOM);
        if (room != NULL) N3DSGml_setVarString(room, "optext", N3DSAm2r_teleRoomName());
        N3DSGml_setGlobalString("tiptext", selected == TELE_AREA ? N3DSAm2r_text(
                "The area (A0 to A8) of the room to go to", "目标房间所在的区域（A0 到 A8）")
            : selected == TELE_ROOM ? N3DSAm2r_text("The room to go to", "要前往的房间")
            : selected == TELE_GO ? N3DSAm2r_text("Go to the chosen room (while playing)", "前往所选房间（游戏进行中）")
            : N3DSAm2r_text("Back to the cheats", "回到金手指"));
        return;
    }
    for (int i = 0; i < CHEAT_COUNT; i++) {
        Instance* row = N3DSAm2r_pageRow(i);
        if (row == NULL) continue;
        N3DSGml_setVarString(row, "optext", gAm2r.cheats[i] ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        N3DSGml_setVar(row, "enabled", (i == CHEAT_MASTER || gAm2r.cheats[CHEAT_MASTER]) ? 1.0 : 0.0);
    }
    N3DSGml_setGlobalString("tiptext", selected >= 0 && selected < CHEAT_COUNT ? N3DSAm2r_cheatTip(selected)
        : selected == CHEATS_PAGE_TELEPORT ? N3DSAm2r_text("Go to any room (while playing)", "前往任意房间（游戏进行中）")
        : N3DSAm2r_text("Back to the extras", "回到额外选项"));
}

// Pages built from the game's own row objects, where the game's pages go (oOptionsMain creates them at (50, 92)):
// Display in place of the game's Display page, Extras in front of the game's Extras page, and Cheats and Teleport from it.
static void N3DSAm2r_openPage(N3DSAm2rPage page, int32_t selected) {
    float x = N3DS_AM2R_PAGE_X, y = N3DS_AM2R_PAGE_Y - 8.0f;
    Instance* title = N3DSGml_create(x, y, gAm2r.objMenuLabel);
    if (title != NULL) N3DSGml_setVarString(title, "text",
        page == PAGE_DISPLAY ? N3DSAm2r_text("Display", "显示")
        : page == PAGE_EXTRAS ? N3DSAm2r_text("Extras", "额外选项")
        : page == PAGE_TELEPORT ? N3DSAm2r_text("Teleport", "传送")
        : N3DSAm2r_text("Cheats", "金手指"));
    gAm2r.pageTitle = title != NULL ? (int32_t) title->instanceId : -1;
    int count = N3DSAm2r_pageRowCount(page);
    for (int i = 0; i < count; i++) {
        bool lrRow = page == PAGE_DISPLAY ? (i == DISPLAY_ROW_FRAMESKIP || i == DISPLAY_ROW_NATIVE ||
            i == DISPLAY_ROW_FPS || i == DISPLAY_ROW_LIGHTING) : page == PAGE_EXTRAS ? false : page == PAGE_TELEPORT ? i <= TELE_ROOM : i < CHEAT_COUNT;
        const char* label = i == count - 1 ? N3DSAm2r_text("Exit", "返回")
            : page == PAGE_DISPLAY ? (i == DISPLAY_ROW_FRAMESKIP ? N3DSAm2r_text("Frameskip", "跳帧")
                : i == DISPLAY_ROW_NATIVE ? N3DSAm2r_text("Fast scripts", "快速脚本")
                : i == DISPLAY_ROW_FPS ? N3DSAm2r_text("FPS counter", "帧率显示")
                : N3DSAm2r_text("Lighting", "光照"))
            : page == PAGE_EXTRAS ? (i == EXTRAS_ROW_GAME ? N3DSAm2r_text("Game extras", "游戏额外内容")
                : N3DSAm2r_text("Cheats", "金手指"))
            : page == PAGE_TELEPORT ? (i == TELE_AREA ? N3DSAm2r_text("Area", "区域")
                : i == TELE_ROOM ? N3DSAm2r_text("Room", "房间") : N3DSAm2r_text("Go", "前往"))
            : i == CHEATS_PAGE_TELEPORT ? N3DSAm2r_text("Teleport", "传送") : N3DSAm2r_cheatLabel(i);
        Instance* row = N3DSGml_create(x, y + N3DS_AM2R_ROW_SEP * (float) (i + 1), lrRow ? gAm2r.objOptionLR : gAm2r.objPauseOption);
        gAm2r.pageRows[i] = row != NULL ? (int32_t) row->instanceId : -1;
        if (row == NULL) continue;
        N3DSGml_setVar(row, "optionid", (double) i);
        N3DSGml_setVarString(row, "label", label);
    }
    N3DSGml_setGlobal("curropt", (double) selected);
    gAm2r.page = page;
    gAm2r.pageFrames = 0;
    gAm2r.pageFirst = 0;
    N3DSAm2r_refreshPage();
}

static void N3DSAm2r_closePage(void) {
    N3DSGml_destroy(N3DSGml_instanceById(gAm2r.pageTitle));
    for (int i = 0; i < N3DSAm2r_pageRowCount(gAm2r.page); i++) N3DSGml_destroy(N3DSAm2r_pageRow(i));
    gAm2r.page = PAGE_NONE;
    N3DSAm2r_saveCheats();
}

static bool N3DSAm2r_pressed(Instance* control, const char* key) {
    char steps[32];
    snprintf(steps, sizeof(steps), "%sPushedSteps", key);
    double down = 0.0, pushed = 1.0;
    return N3DSGml_getVar(control, key, &down) && down > 0.0 && N3DSGml_getVar(control, steps, &pushed) && pushed == 0.0;
}

// ===[ Teleport: where Samus goes ]===

static RValue N3DSAm2r_builtinCall(const char* name, RValue* args, int32_t argCount) {
    VMContext* vm = gAm2r.runner->vmContext;
    ptrdiff_t slot = shgeti(vm->builtinMap, (char*) name);
    if (slot < 0) return RValue_makeReal(0.0);
    return vm->builtinMap[slot].value(vm, args, argCount);
}

// Room for Samus standing at (x, y): no solid in her box, and a floor within reach below. AM2R keeps only the
// instances near the view active, so the area is activated first (the game puts them back to sleep itself).
static bool N3DSAm2r_spotFree(float x, float y) {
    int32_t solid = N3DSGml_objectIndex("oSolid");
    if (solid < 0) return true;
    RValue activate[5] = { RValue_makeReal(x - 80), RValue_makeReal(y - 80), RValue_makeReal(x + 80), RValue_makeReal(y + 260), RValue_makeReal(1.0) };
    RValue r = N3DSAm2r_builtinCall("instance_activate_region", activate, 5);
    RValue_free(&r);
    RValue box[7] = { RValue_makeReal(x - 10), RValue_makeReal(y - 30), RValue_makeReal(x + 10), RValue_makeReal(y + 4),
        RValue_makeReal((double) solid), RValue_makeReal(1.0), RValue_makeReal(1.0) };
    r = N3DSAm2r_builtinCall("collision_rectangle", box, 7);
    bool blocked = RValue_toReal(r) > 0.0;
    RValue_free(&r);
    if (blocked) return false;
    RValue down[7] = { RValue_makeReal(x), RValue_makeReal(y + 4), RValue_makeReal(x), RValue_makeReal(y + 240),
        RValue_makeReal((double) solid), RValue_makeReal(1.0), RValue_makeReal(1.0) };
    r = N3DSAm2r_builtinCall("collision_line", down, 7);
    bool floor = RValue_toReal(r) > 0.0;
    RValue_free(&r);
    return floor;
}

// After a teleport: once the new room has started, Samus goes to a free spot next to a save station or a room exit
// (oGotoRoom triggers on its edges), else the free spot nearest the room's middle; the game put her at the last
// save's spot, which belongs to another room, or left her out when the pause screen did. Made if there is none.
static void N3DSAm2r_placeSamus(void) {
    if (gAm2r.teleTarget < 0 || gAm2r.runner->currentRoomIndex != gAm2r.teleTarget) return;
    if (++gAm2r.teleFrames < 4) return;
    gAm2r.teleTarget = -1;
    float roomWidth = (float) gAm2r.runner->currentRoom->width, roomHeight = (float) gAm2r.runner->currentRoom->height;
    float x = roomWidth / 2, y = roomHeight / 2;
    const char* where = "room centre (no free spot found)";
    bool found = false;
    // Anchors: save stations first, then exits, side exits before top and bottom ones.
    Instance* anchors[24];
    int na = 0;
    for (int pass = 0; pass < 3 && !found; pass++) {
        na = 0;
        if (pass == 0) {
            for (int i = 0; i < gAm2r.objSpotCount; i++) na += N3DSGml_instances(gAm2r.objSpots[i], anchors + na, 24 - na > 8 ? 8 : 24 - na);
        } else if (gAm2r.objGotoRoom >= 0) {
            Instance* exits[24];
            int ne = N3DSGml_instances(gAm2r.objGotoRoom, exits, 24);
            for (int k = 0; k < ne; k++) {
                bool side = exits[k]->x <= 64.0f || exits[k]->x >= roomWidth - 64.0f;
                if (side == (pass == 1)) anchors[na++] = exits[k];
            }
        }
        for (int a = 0; a < na && !found; a++) {
            Instance* e = anchors[a];
            // Inward from the edge it sits on (or beside it), further in until the spot is free.
            static const float kSteps[] = { 48, 64, 96, 128, 32, 160, 0 };
            for (size_t d = 0; d < sizeof(kSteps) / sizeof(kSteps[0]) && !found; d++) {
                float dx = 0.0f, dy = 0.0f;
                if (e->x <= 64.0f) dx = kSteps[d];
                else if (e->x >= roomWidth - 64.0f) dx = -kSteps[d];
                else if (e->y <= 64.0f) dy = kSteps[d];
                else if (e->y >= roomHeight - 64.0f) dy = -kSteps[d];
                else dx = (e->x < roomWidth / 2 ? 1.0f : -1.0f) * kSteps[d];
                if (N3DSAm2r_spotFree(e->x + dx, e->y + dy)) {
                    x = e->x + dx;
                    y = e->y + dy;
                    found = true;
                    where = pass == 0 ? "next to a save station or door" : "next to an exit";
                }
            }
        }
    }
    if (!found) {
        // A grid over the room, nearest the middle first.
        float bestDist = 1.0e30f;
        for (float gy = 32.0f; gy < roomHeight; gy += 32.0f) {
            for (float gx = 32.0f; gx < roomWidth; gx += 32.0f) {
                float dist = (gx - roomWidth / 2) * (gx - roomWidth / 2) + (gy - roomHeight / 2) * (gy - roomHeight / 2);
                if (dist >= bestDist || !N3DSAm2r_spotFree(gx, gy)) continue;
                bestDist = dist;
                x = gx;
                y = gy;
                found = true;
                where = "free spot nearest the room's middle";
            }
        }
    }
    Instance* samus = N3DSGml_anyInstance(gAm2r.objCharacter);
    if (samus == NULL) samus = N3DSGml_create(x, y, gAm2r.objCharacter);
    if (samus == NULL) return;
    samus->x = x;
    samus->y = y;
    N3DSGml_setVar(samus, "hspeed", 0.0);
    N3DSGml_setVar(samus, "vspeed", 0.0);
    logInfo("AM2R: Samus placed at %.0f, %.0f in %s (%s)\n", x, y, gAm2r.runner->currentRoom->name, where);
}

// Lighting off: the light engine isn't drawn (its map isn't built either, see the native); on again: it is.
// A room's light map is a surface its oLightEngine makes on entering the room and frees in its Destroy event, which
// GameMaker doesn't run when a room ends: every room entry left one 512 x 256 surface behind, and a few visits ate
// the 3DS's video memory (the light map then came out smaller, so the dark overlay covered part of the screen, and
// everything slowed down). The previous room's is freed once the new room's exists.
static void N3DSAm2r_freeStaleLightMap(void) {
    static int32_t engine = -2;
    static int32_t lastSurface = -1;
    if (engine == -2) engine = N3DSGml_objectIndex("oLightEngine");
    if (engine < 0) return;
    Instance* list[1];
    double surf = -1.0;
    int32_t current = -1;
    if (N3DSGml_instances(engine, list, 1) > 0 && N3DSGml_getVar(list[0], "surf", &surf)) current = (int32_t) surf;
    if (lastSurface >= 0 && current >= 0 && current != lastSurface) {
        Renderer* renderer = gAm2r.runner->renderer;
        if (renderer != NULL && renderer->vtable->surfaceExists(renderer, lastSurface)) renderer->vtable->surfaceFree(renderer, lastSurface);
    }
    if (current >= 0) lastSurface = current;
}

// The pause screens (items, log) are laid out for the game's 426-wide picture, and the bottom screen is 320 wide: they
// came out cropped on both sides. While paused the game is told it has no widescreen, so it lays them out for 320.
static void N3DSAm2r_pauseLayout(void) {
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    if (control == NULL) return;
    if (N3DSPause_isPaused()) {
        if (gAm2r.pauseWidescreenSaved) return;
        double ws = 0.0, space = 0.0;
        if (!N3DSGml_getVar(control, "widescreen", &ws) || ws < 0.5) return;
        N3DSGml_getVar(control, "widescreen_space", &space);
        gAm2r.pauseWidescreen = ws;
        gAm2r.pauseWidescreenSpace = space;
        gAm2r.pauseWidescreenSaved = true;
        N3DSGml_setVar(control, "widescreen", 0.0);
        N3DSGml_setVar(control, "widescreen_space", 0.0);
    } else if (gAm2r.pauseWidescreenSaved) {
        gAm2r.pauseWidescreenSaved = false;
        N3DSGml_setVar(control, "widescreen", gAm2r.pauseWidescreen);
        N3DSGml_setVar(control, "widescreen_space", gAm2r.pauseWidescreenSpace);
    }
}

static void N3DSAm2r_applyLighting(void) {
    N3DSAm2r_pauseLayout();
    N3DSAm2r_freeStaleLightMap();
    static int32_t engine = -2;
    static int lastApplied = -1;
    if (engine == -2) engine = N3DSGml_objectIndex("oLightEngine");
    if (engine < 0) return;
    bool on = N3DSAm2rNative_lighting();
    Instance* list[4];
    int32_t n = N3DSGml_instances(engine, list, 4);
    if (n == 0 && lastApplied == (on ? 1 : 0)) return;
    for (int32_t i = 0; i < n; i++) list[i]->visible = on;
    lastApplied = on ? 1 : 0;
}

static void N3DSAm2r_updatePage(void) {
    int count = N3DSAm2r_pageRowCount(gAm2r.page);
    // The page's instances go with the room (leaving the pause screen): nothing to go back to then.
    if (N3DSAm2r_pageRow(count - 1) == NULL) {
        gAm2r.page = PAGE_NONE;
        N3DSAm2r_saveCheats();
        return;
    }
    // Like the game's pages: input after a few frames, so the press that opened the page doesn't act on it.
    if (++gAm2r.pageFrames < 5) return;
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    if (control == NULL) return;
    double curropt = 0.0;
    N3DSGml_getGlobal("curropt", -1, &curropt);
    int32_t selected = (int32_t) curropt;
    int32_t exitRow = count - 1;
    if (selected < 0 || selected > exitRow) selected = 0;
    bool menu1 = N3DSAm2r_pressed(control, "kMenu1");
    bool left = N3DSAm2r_pressed(control, "kLeft"), right = N3DSAm2r_pressed(control, "kRight");
    if (N3DSAm2r_pressed(control, "kDown")) {
        selected = selected >= exitRow ? 0 : selected + 1;
        N3DSAm2r_sfx(gAm2r.sndMenuMove);
    } else if (N3DSAm2r_pressed(control, "kUp")) {
        selected = selected <= 0 ? exitRow : selected - 1;
        N3DSAm2r_sfx(gAm2r.sndMenuMove);
    } else if (N3DSAm2r_pressed(control, "kMenu2") || (selected == exitRow && menu1)) {
        // Back: from Teleport to Cheats, from Cheats to Extras, from Extras and Display to the options.
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
        N3DSAm2rPage page = gAm2r.page;
        N3DSAm2r_closePage();
        if (page == PAGE_TELEPORT) N3DSAm2r_openPage(PAGE_CHEATS, CHEATS_PAGE_TELEPORT);
        else if (page == PAGE_CHEATS) N3DSAm2r_openPage(PAGE_EXTRAS, EXTRAS_ROW_CHEATS);
        else N3DSGml_create(N3DS_AM2R_PAGE_X, N3DS_AM2R_PAGE_Y, gAm2r.objOptionsMain);
        return;
    } else if (gAm2r.page == PAGE_EXTRAS && selected == EXTRAS_ROW_CHEATS && menu1) {
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
        N3DSAm2r_closePage();
        N3DSAm2r_openPage(PAGE_CHEATS, 0);
        return;
    } else if (gAm2r.page == PAGE_EXTRAS && selected == EXTRAS_ROW_GAME && menu1) {
        // The game's own Extras page, on top of ours (its Exit goes to the options).
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
        N3DSAm2r_closePage();
        gAm2r.gameExtras = true;
        N3DSGml_create(N3DS_AM2R_PAGE_X, N3DS_AM2R_PAGE_Y, gAm2r.objOptionsExtras);
        return;
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_FRAMESKIP && (menu1 || left || right)) {
        N3DS_setFrameskip(!N3DS_getFrameskip());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_NATIVE && (menu1 || left || right) &&
               N3DSAm2rNative_available() > 0) {
        N3DSAm2rNative_setEnabled(!N3DSAm2rNative_enabled());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_LIGHTING && (menu1 || left || right)) {
        N3DSAm2rNative_setLighting(!N3DSAm2rNative_lighting());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_FPS && (menu1 || left || right)) {
        N3DS_setFpsCounter(!N3DS_getFpsCounter());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_CHEATS && selected == CHEATS_PAGE_TELEPORT && menu1) {
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
        N3DSAm2r_closePage();
        N3DSAm2r_openPage(PAGE_TELEPORT, TELE_AREA);
        return;
    } else if (gAm2r.page == PAGE_TELEPORT && (selected == TELE_AREA || selected == TELE_ROOM) && (menu1 || left || right)) {
        N3DSAm2r_teleStep(selected, left ? -1 : 1);
        N3DSAm2r_sfx(gAm2r.sndMenuMove);
    } else if (gAm2r.page == PAGE_TELEPORT && selected == TELE_GO && menu1) {
        // Only with a game running: the rooms expect Samus and the game's state.
        int32_t n = gAm2r.teleRoomCount[gAm2r.teleArea];
        if (n > 0 && gAm2r.gameRunning) {
            N3DSAm2r_sfx(gAm2r.sndMenuSel);
            // The game is paused (global.ingame 0): it plays again in the new room, where Samus is put on a spot.
            N3DSGml_setGlobal("ingame", 1.0);
            gAm2r.teleTarget = gAm2r.teleRooms[gAm2r.teleArea][gAm2r.teleRoom % n];
            gAm2r.teleFrames = 0;
            gAm2r.runner->pendingRoom = gAm2r.teleTarget;
            logInfo("AM2R: teleport to %s\n", N3DSAm2r_teleRoomName());
            return;
        }
    } else if (gAm2r.page == PAGE_CHEATS && selected < CHEAT_COUNT && (menu1 || left || right)) {
        gAm2r.cheats[selected] = !gAm2r.cheats[selected];
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    }
    N3DSGml_setGlobal("curropt", (double) selected);
    N3DSAm2r_refreshPage();
}

// ===[ Cheat effects ]===

static void N3DSAm2r_setAll(int32_t objectIndex, const char* var, double value) {
    if (objectIndex < 0) return;
    Instance* list[32];
    int32_t n = N3DSGml_instances(objectIndex, list, 32);
    for (int32_t i = 0; i < n; i++) N3DSGml_setVar(list[i], var, value);
}

static void N3DSAm2r_fillGlobal(const char* var, const char* maxVar) {
    double max = 0.0;
    if (N3DSGml_getGlobal(maxVar, -1, &max)) N3DSGml_setGlobal(var, max);
}

// All Power-ups: every item that isn't collected is switched on while the cheat is; switching it off (or the master
// switch) puts those back as they were, so a save made without the cheat keeps its own items.
static void N3DSAm2r_applyPowerups(void) {
    bool playing = gAm2r.gameRunning; // also while paused (global.ingame is 0 then)
    if (N3DSAm2r_cheat(CHEAT_POWERUPS)) {
        if (!playing && !gAm2r.itemsForced) return;
        if (!gAm2r.itemsForced) {
            gAm2r.itemsForced = true;
            memset(gAm2r.itemChanged, 0, sizeof(gAm2r.itemChanged));
            logInfo("AM2R: All Power-ups on\n");
        }
        for (int i = 0; i < N3DS_AM2R_ITEMS; i++) {
            double v = 0.0;
            if (!N3DSGml_getGlobal("item", i, &v)) continue;
            if (v == 0.0 && !gAm2r.itemChanged[i]) {
                gAm2r.itemBackup[i] = v;
                gAm2r.itemChanged[i] = true;
            }
            if (v == 0.0) N3DSGml_setGlobalElement("item", i, 1.0);
        }
    } else if (gAm2r.itemsForced) {
        gAm2r.itemsForced = false;
        // Back at the title screen the items are another game's: leave them.
        if (!gAm2r.gameRunning) return;
        for (int i = 0; i < N3DS_AM2R_ITEMS; i++) {
            if (gAm2r.itemChanged[i]) N3DSGml_setGlobalElement("item", i, gAm2r.itemBackup[i]);
        }
    }
}

static void N3DSAm2r_applyCheats(void) {
    N3DSAm2r_applyPowerups();
    if (!gAm2r.cheats[CHEAT_MASTER]) return;
    double inGame = 0.0;
    if (!N3DSGml_getGlobal("ingame", -1, &inGame) || inGame == 0.0) return;
    // Other drains (heat, Metroids latched on) go straight to samushealth: filled after every step.
    if (N3DSAm2r_cheat(CHEAT_HEALTH)) N3DSAm2r_fillGlobal("samushealth", "maxhealth");
    if (N3DSAm2r_cheat(CHEAT_AMMO)) {
        N3DSAm2r_fillGlobal("missiles", "maxmissiles");
        N3DSAm2r_fillGlobal("smissiles", "maxsmissiles");
        N3DSAm2r_fillGlobal("pbombs", "maxpbombs");
    }
    if (N3DSAm2r_cheat(CHEAT_BEAM)) N3DSAm2r_setAll(gAm2r.objBeam, "damage", N3DS_AM2R_SUPER_DAMAGE);
    if (N3DSAm2r_cheat(CHEAT_BOMBS)) {
        N3DSAm2r_setAll(gAm2r.objBombExpl, "damage", N3DS_AM2R_SUPER_DAMAGE);
        N3DSAm2r_setAll(gAm2r.objPBombExpl, "damage", N3DS_AM2R_SUPER_DAMAGE);
    }
    if (N3DSAm2r_cheat(CHEAT_MISSILES)) {
        N3DSAm2r_setAll(gAm2r.objMissile, "damage", N3DS_AM2R_SUPER_DAMAGE);
        N3DSAm2r_setAll(gAm2r.objMissileExpl, "damage", N3DS_AM2R_SUPER_DAMAGE);
        // Metroids take a fixed amount per missile: one hit is enough.
        for (int m = 0; m < 4; m++) {
            if (gAm2r.objMetroids[m] < 0) continue;
            Instance* list[8];
            int32_t n = N3DSGml_instances(gAm2r.objMetroids[m], list, 8);
            for (int32_t i = 0; i < n; i++) {
                double health = 0.0;
                if (N3DSGml_getVar(list[i], "myhealth", &health) && health > 1.0) N3DSGml_setVar(list[i], "myhealth", 1.0);
            }
        }
    }
}

// ===[ Water / lava ripple filter ]===

static void N3DSAm2r_dropWaterFilter(void) {
    if (gAm2r.objWaterFX < 0) return;
    Instance* filter = N3DSGml_anyInstance(gAm2r.objWaterFX);
    if (filter == NULL) return;
    double surface = -1.0;
    Renderer* renderer = gAm2r.runner->renderer;
    if (N3DSGml_getVar(filter, "mysurf", &surface) && surface >= 0.0 && renderer != NULL) {
        renderer->vtable->surfaceFree(renderer, (int32_t) surface);
    }
    N3DSGml_destroy(filter);
}

int32_t N3DSAm2r_gameWidth(void) {
    if (!gAm2r.active) return 0;
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    double widescreen = 0.0, space = 0.0;
    if (control == NULL || !N3DSGml_getVar(control, "widescreen", &widescreen) || widescreen < 0.5) return 0;
    if (!N3DSGml_getVar(control, "widescreen_space", &space) || space <= 0.0) return 0;
    return 320 + (int32_t) space;
}

// AM2R 1.5 creates its widescreen surface once, from the widescreen view's width, on its first frame, before the
// widescreen width is set: 320 wide, so only 320 of the 426 columns ever showed. On a PC the surface is lost and made
// again (window and display changes at startup); here it would never be. A wrong-sized one is freed for the game to
// make again.
static void N3DSAm2r_checkWidescreenSurface(void) {
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    double widescreen = 0.0, space = 0.0, surface = -1.0;
    if (control == NULL || !N3DSGml_getVar(control, "widescreen", &widescreen) || widescreen < 0.5) return;
    if (!N3DSGml_getVar(control, "widescreen_space", &space) || !N3DSGml_getVar(control, "widescreen_surface", &surface) ||
        surface < 0.0) return;
    Renderer* renderer = gAm2r.runner->renderer;
    int32_t id = (int32_t) surface;
    if (renderer == NULL || !renderer->vtable->surfaceExists(renderer, id)) return;
    float width = Renderer_getSurfaceWidth(renderer, id);
    if ((int32_t) width == 320 + (int32_t) space) return;
    logInfo("AM2R: widescreen surface %d is %.0f wide, not %d: made again\n", (int) id, (double) width, 320 + (int) space);
    renderer->vtable->surfaceFree(renderer, id);
}

// Only the spin-jump loop the game tracks (oCharacter.spinjump_sound) may play. Entering water, the character step
// stops the loop, picks the sound again before setting inwater (so the same one), and later in the same step finds it
// not playing and restarts it; from the next step it tracks the underwater sound, and the restarted loop played until
// Samus left the water. Any other spin loop still playing after the step is stopped here.
static void N3DSAm2r_stopStraySpinLoops(void) {
    if (gAm2r.objCharacter < 0) return;
    Instance* samus = N3DSGml_firstInstance(gAm2r.objCharacter);
    double current = -1.0;
    if (samus == NULL || !N3DSGml_getVar(samus, "spinjump_sound", &current)) return;
    AudioSystem* audio = gAm2r.runner->audioSystem;
    if (audio == NULL) return;
    repeat(5, i) {
        int32_t sound = gAm2r.sndSpin[i];
        if (sound < 0 || sound == (int32_t) current) continue;
        if (audio->vtable->isPlaying(audio, sound)) audio->vtable->stopSound(audio, sound);
    }
}

// The pause screen without its fades: its foreground (oSS_Fg: the frame sliding in, then fading in, and fading out
// before the game resumes) and background (oSS_Bg) are put at the end of each fade as soon as it starts.
static void N3DSAm2r_skipPauseFades(void) {
    Instance* fg = gAm2r.objSSFg >= 0 ? N3DSGml_firstInstance(gAm2r.objSSFg) : NULL;
    if (fg != NULL) {
        double fadein = 0.0, fadeout = 0.0;
        N3DSGml_getVar(fg, "fadein", &fadein);
        N3DSGml_getVar(fg, "fadeout", &fadeout);
        if (fadeout != 0.0) {
            N3DSGml_setVar(fg, "ealpha", 0.0);
        } else if (fadein != 0.0) {
            N3DSGml_setVar(fg, "rectoffset", 0.0);
            N3DSGml_setVar(fg, "ealpha", 1.0);
            N3DSGml_setVar(fg, "fadein", 0.0);
        }
    }
    Instance* bg = gAm2r.objSSBg >= 0 ? N3DSGml_firstInstance(gAm2r.objSSBg) : NULL;
    if (bg != NULL) {
        double fadein = 0.0, fadeout = 0.0;
        N3DSGml_getVar(bg, "fadein", &fadein);
        N3DSGml_getVar(bg, "fadeout", &fadeout);
        if (fadeout != 0.0) {
            bg->imageAlpha = 0.0f;
        } else if (fadein != 0.0) {
            bg->imageAlpha = 1.0f;
            N3DSGml_setVar(bg, "fadein", 0.0);
        }
    }
}

#define N3DS_AM2R_TOP_W 400
#define N3DS_AM2R_TOP_H 240

// "Paused" over the frozen game on the top screen while the pause screen is on the bottom one: a semi-transparent
// black bar and the game's own item-message text (draw_cool_text in global.fontGUI, its colours).
void N3DSAm2r_drawPausedBanner(Renderer* renderer) {
    if (!gAm2r.active || renderer == NULL) return;
    N3DSRenderer_beginTopScreenGUI(renderer, N3DS_AM2R_TOP_W, N3DS_AM2R_TOP_H);
    const float barH = 36.0f, barY = ((float) N3DS_AM2R_TOP_H - barH) * 0.5f;
    renderer->vtable->drawRectangle(renderer, 0.0f, barY, (float) N3DS_AM2R_TOP_W, barY + barH, 0x000000, 0.6f, false);
    double font = -1.0;
    if (gAm2r.drawCoolText >= 0 && N3DSGml_getGlobal("fontGUI", -1, &font)) {
        int32_t savedFont = renderer->drawFont, savedHalign = renderer->drawHalign, savedValign = renderer->drawValign;
        uint32_t savedColor = renderer->drawColor;
        float savedAlpha = renderer->drawAlpha;
        renderer->drawFont = (int32_t) font;
        renderer->drawHalign = 1;
        renderer->drawValign = 1;
        RValue args[7] = {
            RValue_makeReal((GMLReal) N3DS_AM2R_TOP_W * 0.5), RValue_makeReal((GMLReal) N3DS_AM2R_TOP_H * 0.5),
            RValue_makeString("Paused"), RValue_makeReal(16777215.0), RValue_makeReal(16776960.0),
            RValue_makeReal(8388608.0), RValue_makeReal(1.0),
        };
        RValue result = N3DSGml_callScript(gAm2r.drawCoolText, args, 7);
        RValue_free(&result);
        renderer->drawFont = savedFont;
        renderer->drawHalign = savedHalign;
        renderer->drawValign = savedValign;
        renderer->drawColor = savedColor;
        renderer->drawAlpha = savedAlpha;
    }
    N3DSRenderer_endTopScreenGUI(renderer);
}

void N3DSAm2r_update(void) {
    if (!gAm2r.active) return;
    N3DSAm2r_stopStraySpinLoops();
    N3DSAm2r_skipPauseFades();
    N3DSAm2r_checkWidescreenSurface();
    N3DSAm2r_dropWaterFilter();
    N3DSAm2r_hideKeyboardRow();
    double inGame = 0.0;
    if (N3DSGml_getGlobal("ingame", -1, &inGame) && inGame != 0.0) gAm2r.gameRunning = true;
    else if (gAm2r.runner->currentRoomIndex == gAm2r.roomTitle) gAm2r.gameRunning = false;
    if (gAm2r.page != PAGE_NONE) {
        N3DSAm2r_updatePage();
    } else {
        Instance* display = gAm2r.objOptionsDisplay >= 0 ? N3DSGml_anyInstance(gAm2r.objOptionsDisplay) : NULL;
        Instance* extras = gAm2r.objOptionsExtras >= 0 ? N3DSGml_anyInstance(gAm2r.objOptionsExtras) : NULL;
        if (extras == NULL) gAm2r.gameExtras = false;
        if (display != NULL) {
            N3DSGml_destroy(display);
            N3DSAm2r_openPage(PAGE_DISPLAY, DISPLAY_ROW_FRAMESKIP);
        } else if (extras != NULL && !gAm2r.gameExtras) {
            N3DSGml_destroy(extras);
            N3DSAm2r_openPage(PAGE_EXTRAS, EXTRAS_ROW_GAME);
        }
    }
    N3DSAm2r_applyLighting();
    N3DSAm2r_placeSamus();
    N3DSAm2r_applyCheats();
}
