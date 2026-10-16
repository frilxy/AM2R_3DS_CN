#include "n3ds_am2r.h"
#include "n3ds_am2r_native.h"

#include "n3ds_font_replace.h"
#include "n3ds_gml.h"
#include "n3ds_platform_config.h"
#include "n3ds_renderer.h"
#include "n3ds_screen.h"

#include "../ini.h"
#include "../log.h"
#include "../renderer.h"
#include "../utils.h"
#include "../vm.h"

#include <stb/ds/stb_ds.h>
#include <stb_image.h>
#include <stdio.h>
#include <stdlib.h>
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

enum { CHEAT_MASTER, CHEAT_HEALTH, CHEAT_AMMO, CHEAT_BEAM, CHEAT_BOMBS, CHEAT_MISSILES, CHEAT_COUNT };
enum { DISPLAY_ROW_SCREEN, DISPLAY_ROW_FRAMESKIP, DISPLAY_ROW_NATIVE, DISPLAY_ROW_FPS, DISPLAY_ROW_CHEATS, DISPLAY_ROW_EXIT, DISPLAY_ROW_COUNT };

typedef enum { PAGE_NONE, PAGE_DISPLAY, PAGE_CHEATS } N3DSAm2rPage;

static const char* const kCheatNames[CHEAT_COUNT] = { "master", "health", "ammo", "beam", "bombs", "missiles" };
static const char* const kCheatLabels[CHEAT_COUNT] = {
    "Cheats", "Unlimited Health", "Unlimited Ammo", "Super Beam", "Super Bombs", "Super Missiles",
};
static const char* const kCheatLabelsZh[CHEAT_COUNT] = {
    "作弊", "无限生命", "无限弹药", "强化光束", "强化炸弹", "强化导弹",
};
static const char* const kCheatTips[CHEAT_COUNT] = {
    "Turn all cheats on or off",
    "Energy stays full",
    "Missiles, Super Missiles and Power Bombs stay full",
    "Beam shots destroy anything they hit",
    "Bombs and Power Bombs destroy anything they hit",
    "Missiles destroy anything they hit, Metroids too",
};
static const char* const kCheatTipsZh[CHEAT_COUNT] = {
    "打开或关闭全部作弊",
    "能量始终保持全满",
    "导弹、超级导弹与强力炸弹始终保持全满",
    "光束击中任何目标都会将其摧毁",
    "炸弹与强力炸弹击中任何目标都会将其摧毁",
    "导弹击中任何目标都会将其摧毁，对 Metroid 也有效",
};

typedef struct {
    const char* name;
    BuiltinFunc hook;
    int32_t script; // the game's own script, called by the hook
} N3DSAm2rHook;

typedef struct {
    Runner* runner;
    bool active;
    int32_t objOptionsMain, objOptionsDisplay, objOptionsControl, objControl;
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
    int32_t pageRows[CHEAT_COUNT + 1 > DISPLAY_ROW_COUNT ? CHEAT_COUNT + 1 : DISPLAY_ROW_COUNT];
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

static RValue hookGetText(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    const char* section = argCount > 0 && args[0].type == RVALUE_STRING ? args[0].string : "";
    const char* key = argCount > 1 && args[1].type == RVALUE_STRING ? args[1].string : "";
    if (strcmp(section, "OptionsMain") == 0 && strcmp(key, "Display_Tip") == 0) {
        return RValue_makeOwnedString(safeStrdup("Screen mode and cheats"));
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

// ===[ Language packs: font_replace / background_replace ]===
// Nothing in Butterscotch implemented either built-in, and AM2R's language packs are made of exactly these two calls:
// font_replace swaps a font for glyphs out of a TTF, background_replace swaps the title logo for a PNG. Without them
// the language files load, the language name is read correctly, and everything still draws in English.
// Neither has a game script behind it, so unlike the hooks above these are installed by name (see N3DSAm2r_init).

// font_replace(font, ttf, size, bold, italic). The game passes more arguments than GameMaker documents (two extra
// numbers) and only the first three matter here.
static RValue hookFontReplace(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 3 || args[1].type != RVALUE_STRING || args[1].string == NULL) return RValue_makeUndefined();
    int32_t fontIndex = RValue_toInt32(args[0]);
    float emSize = (float) RValue_toReal(args[2]);
    bool bold = argCount > 3 && RValue_toBool(args[3]);
    bool italic = argCount > 4 && RValue_toBool(args[4]);
    if (!N3DSFontReplace_apply(gAm2r.runner, fontIndex, args[1].string, emSize, bold, italic)) {
        logInfo("AM2R: font_replace(%d, %s, %g) was not applied\n", (int) fontIndex, args[1].string, (double) emSize);
    }
    return RValue_makeUndefined();
}

// background_replace(background, png, removeback, smooth, xorigin, yorigin). The language packs use it for the title
// logo (lang/titles/*.png). The PNG is decoded and uploaded whole; the background is then drawn at the image's real
// size, which is what makes the logo line up with the title screen.
static RValue hookBackgroundReplace(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount < 2 || args[1].type != RVALUE_STRING || args[1].string == NULL) return RValue_makeUndefined();
    int32_t bgIndex = RValue_toInt32(args[0]);
    DataWin* dw = gAm2r.runner != NULL ? gAm2r.runner->dataWin : NULL;
    if (dw == NULL || bgIndex < 0 || (uint32_t) bgIndex >= dw->bgnd.count) return RValue_makeUndefined();

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(args[1].string, &width, &height, &channels, 4);
    if (pixels == NULL || width <= 0 || height <= 0) {
        logInfo("AM2R: background_replace could not read %s\n", args[1].string);
        if (pixels != NULL) stbi_image_free(pixels);
        return RValue_makeUndefined();
    }

    int32_t tpagIndex = N3DSRenderer_allocRuntimeImage(gAm2r.runner->renderer, pixels, (uint32_t) width, (uint32_t) height);
    stbi_image_free(pixels);
    if (tpagIndex < 0) return RValue_makeUndefined();

    N3DSRenderer_setRuntimeTPAGContent(gAm2r.runner->renderer, tpagIndex, (uint32_t) width, (uint32_t) height);
    // A background draws through its tpagIndex, so pointing it at the new TPAG is the whole replacement. The TPAG's
    // own source/bounding sizes were set above, to the PNG's real size.
    dw->bgnd.backgrounds[bgIndex].tpagIndex = tpagIndex;
    logInfo("AM2R: background %d -> %s (%dx%d, tpag %d)\n", (int) bgIndex, args[1].string, width, height, (int) tpagIndex);
    return RValue_makeUndefined();
}

// ===[ Screen mode ]===

// The screen modes the Display page offers, in order (Stretch first: the default).
static const N3DSScreenMode kScreenModes[] = { N3DS_SCREEN_STRETCH, N3DS_SCREEN_1X, N3DS_SCREEN_2X, N3DS_SCREEN_WIDE };
static const char* const kScreenModeNames[] = { "Stretch", "1x", "2x", "Wide" };
#define N3DS_AM2R_SCREEN_MODES ((int) (sizeof(kScreenModes) / sizeof(kScreenModes[0])))

static int N3DSAm2r_screenModeSlot(void) {
    N3DSScreenMode mode = N3DS_getScreenMode();
    for (int i = 0; i < N3DS_AM2R_SCREEN_MODES; i++) {
        if (kScreenModes[i] == mode) return i;
    }
    return 0;
}

// ===[ Languages ]===
// lang/languages.txt holds one language file per line; a line whose file is missing is skipped, and the line's index
// is what config.ini's [Screen] Language selects (that is the game's own global.currentlanguage). The game reads both
// itself, so switching language needs no code here -- but the 3DS front end's own pages are not in any language file,
// so they need to know which language is active to follow it.

#define N3DS_AM2R_LANGUAGES 16

typedef struct {
    char* file;
    char* name;
} N3DSAm2rLanguage;

static N3DSAm2rLanguage gLanguages[N3DS_AM2R_LANGUAGES];
static int gLanguageCount = 0;

static char* N3DSAm2r_trimLine(char* line) {
    while (*line == ' ' || *line == '\t') line++;
    char* end = line + strlen(line);
    while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) end--;
    *end = '\0';
    return line;
}

// Reads one "Key=Value" out of a language file's section. The game's own reader is ini_read_string, and its files
// start with a UTF-8 BOM, which ini.c skips.
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
        for (const char* p = value; *p != '\0' && at + 1u < outSize; p++) out[at++] = (*p == '#') ? ' ' : *p;
        out[at] = '\0';
    }
    Ini_free(ini);
}

static void N3DSAm2r_loadLanguages(void) {
    for (int i = 0; i < gLanguageCount; i++) {
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
        logInfo("AM2R: language %d: %s (%s)\n", gLanguageCount, file, gLanguages[gLanguageCount].name);
        gLanguageCount++;
    }
    fclose(f);
}

// The selected language's index. config.ini's [Screen] Language is the game's own setting; global.currentlanguage is
// what it has actually applied, so it wins while the game is running.
static int32_t N3DSAm2r_languageSlot(void) {
    double current = 0.0;
    if (N3DSGml_getGlobal("currentlanguage", -1, &current) && current >= 0.0) return (int32_t) current;
    char text[64];
    N3DSAm2r_readIniString(N3DS_SD_DIR "config.ini", "Screen", "Language", text, sizeof(text));
    return text[0] != '\0' ? (int32_t) strtol(text, NULL, 10) : 0;
}

// True while a language whose file starts with this prefix is selected. Used to pick the front end's own strings.
static bool N3DSAm2r_languageIs(const char* prefix) {
    int32_t slot = N3DSAm2r_languageSlot();
    if (slot < 0 || slot >= gLanguageCount) return false;
    const char* file = gLanguages[slot].file;
    return file != NULL && strncmp(file, prefix, strlen(prefix)) == 0;
}

// The front end's pages in English and Chinese. Only Chinese is translated: the user asked for those two languages,
// and a wrong translation is worse than the English string.
static const char* N3DSAm2r_text(const char* english, const char* chinese) {
    return N3DSAm2r_languageIs("schinese") ? chinese : english;
}

// Every Chinese character the front end draws. They are in no language file, so without this they would be missing
// from the atlas the loader builds and would draw as blanks. Call before oControl's first font_replace.
static void N3DSAm2r_registerPortGlyphs(void) {
    // These must be the same strings the pages draw (the kCheat*Zh tables above and the N3DSAm2r_text calls in
    // N3DSAm2r_refreshPage and N3DSAm2r_openPage): a character that only appears here is wasted atlas space, and one
    // that is missing renders as nothing at all.
    static const char* const kPortChinese[] = {
        // Titles, row labels and row values.
        "显示", "作弊", "返回", "画面模式", "跳帧", "快速脚本", "帧率显示", "开", "关", "不可用",
        "回到选项", "此版本的游戏不支持", "左下角的小型帧率计数器", "无限生命和弹药，更强的武器",
        // The Display page's tips.
        "拉伸填满屏幕；1x/2x 保持方形像素；宽屏显示更多场景",
        "保持完整游戏速度下稳定的 30 帧（隔帧绘制）",
        "繁重的游戏脚本改用内置代码运行：更快，行为相同",
        // The Cheats page's rows and tips.
        "无限生命", "无限弹药", "强化光束", "强化炸弹", "强化导弹",
        "打开或关闭全部作弊", "能量始终保持全满",
        "导弹、超级导弹与强力炸弹始终保持全满",
        "光束击中任何目标都会将其摧毁",
        "炸弹与强力炸弹击中任何目标都会将其摧毁",
        "导弹击中任何目标都会将其摧毁，对 Metroid 也有效",
        // The screen mode names (this row is the game's own, filled in with these).
        "拉伸", "宽屏",
    };
    size_t total = 0;
    for (size_t i = 0; i < sizeof(kPortChinese) / sizeof(kPortChinese[0]); i++) {
        N3DSFontReplace_addExtraText(kPortChinese[i]);
        total += strlen(kPortChinese[i]);
    }
    logInfo("AM2R: %d languages, %u bytes of front end strings in %s\n",
        gLanguageCount, (unsigned) total, N3DSAm2r_languageIs("schinese") ? "Chinese" : "English");
}

// ===[ Cheats file (and the screen mode) ]===

static void N3DSAm2r_loadCheats(void) {
    FILE* f = fopen(N3DS_AM2R_CHEATS_FILE, "r");
    if (f == NULL) return;
    char line[64];
    while (fgets(line, sizeof(line), f) != NULL) {
        char name[32];
        int value = 0;
        if (sscanf(line, "%31[^=]=%d", name, &value) != 2) continue;
        if (strcmp(name, "screen") == 0 && value >= 0 && value < N3DS_AM2R_SCREEN_MODES) N3DS_setScreenMode(kScreenModes[value]);
        if (strcmp(name, "frameskip") == 0) N3DS_setFrameskip(value != 0);
        if (strcmp(name, "native") == 0) N3DSAm2rNative_setEnabled(value != 0);
        if (strcmp(name, "fps") == 0) N3DS_setFpsCounter(value != 0);
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
    fprintf(f, "screen=%d\n", N3DSAm2r_screenModeSlot());
    fprintf(f, "frameskip=%d\n", N3DS_getFrameskip() ? 1 : 0);
    fprintf(f, "native=%d\n", N3DSAm2rNative_enabled() ? 1 : 0);
    fprintf(f, "fps=%d\n", N3DS_getFpsCounter() ? 1 : 0);
    fclose(f);
}

static bool N3DSAm2r_cheat(int which) {
    return gAm2r.cheats[CHEAT_MASTER] && gAm2r.cheats[which];
}

// ===[ Init ]===

void N3DSAm2r_init(Runner* runner) {
    memset(&gAm2r, 0, sizeof(gAm2r));
    gAm2r.runner = runner;
    gAm2r.lastControlPage = -1;
    gAm2r.objOptionsMain = N3DSGml_objectIndex("oOptionsMain");
    gAm2r.objOptionsDisplay = N3DSGml_objectIndex("oOptionsDisplay");
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
    // font_replace and background_replace have no game script behind them (the loop above only wraps functions the
    // game also defines), so their cached builtins are replaced by name. That must happen before oControl's Create
    // event, which is where the language pack calls them.
    for (uint32_t i = 0; i < vm->funcCallCacheCount; i++) {
        const char* name = dataWin->func.functions[i].name;
        if (name == NULL) continue;
        if (strcmp(name, "font_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookFontReplace;
            logInfo("AM2R: font_replace goes through the 3DS front end\n");
        } else if (strcmp(name, "background_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookBackgroundReplace;
            logInfo("AM2R: background_replace goes through the 3DS front end\n");
        }
    }
    // Fast scripts default to on; cheats.ini can turn them off.
    N3DSAm2rNative_setEnabled(true);
    N3DSAm2r_loadCheats();
    N3DSAm2r_loadLanguages();
    // The 3DS front end's own pages are not in any language file, so their characters have to be offered to the font
    // loader before oControl builds the atlas, or they render as blanks.
    N3DSAm2r_registerPortGlyphs();
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
    return page == PAGE_DISPLAY ? DISPLAY_ROW_COUNT : CHEAT_COUNT + 1;
}

static void N3DSAm2r_refreshPage(void) {
    double curropt = 0.0;
    N3DSGml_getGlobal("curropt", -1, &curropt);
    int32_t selected = (int32_t) curropt;
    if (gAm2r.page == PAGE_DISPLAY) {
        Instance* screen = N3DSAm2r_pageRow(DISPLAY_ROW_SCREEN);
        if (screen != NULL) N3DSGml_setVarString(screen, "optext", kScreenModeNames[N3DSAm2r_screenModeSlot()]);
        Instance* frameskip = N3DSAm2r_pageRow(DISPLAY_ROW_FRAMESKIP);
        if (frameskip != NULL) N3DSGml_setVarString(frameskip, "optext", N3DS_getFrameskip() ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        Instance* native = N3DSAm2r_pageRow(DISPLAY_ROW_NATIVE);
        if (native != NULL) {
            N3DSGml_setVarString(native, "optext", N3DSAm2rNative_available() == 0 ? N3DSAm2r_text("N/A", "不可用")
                : N3DSAm2rNative_enabled() ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        }
        Instance* fps = N3DSAm2r_pageRow(DISPLAY_ROW_FPS);
        if (fps != NULL) N3DSGml_setVarString(fps, "optext", N3DS_getFpsCounter() ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        const char* tip = selected == DISPLAY_ROW_SCREEN
                ? N3DSAm2r_text("Stretch fills the screen; 1x and 2x keep square pixels; Wide shows more of the room",
                    "拉伸填满屏幕；1x/2x 保持方形像素；宽屏显示更多场景")
            : selected == DISPLAY_ROW_FRAMESKIP
                ? N3DSAm2r_text("A steady 30 fps at full game speed (draws every other frame)",
                    "保持完整游戏速度下稳定的 30 帧（隔帧绘制）")
            : selected == DISPLAY_ROW_NATIVE
                ? (N3DSAm2rNative_available() == 0 ? N3DSAm2r_text("Not available for this version of the game", "此版本的游戏不支持")
                    : N3DSAm2r_text("Busy game scripts run as built-in code: faster, same behaviour",
                        "繁重的游戏脚本改用内置代码运行：更快，行为相同"))
            : selected == DISPLAY_ROW_FPS ? N3DSAm2r_text("A small frame rate counter in the bottom-left corner",
                    "左下角的小型帧率计数器")
            : selected == DISPLAY_ROW_CHEATS ? N3DSAm2r_text("Unlimited health and ammo, stronger weapons",
                    "无限生命和弹药，更强的武器")
            : N3DSAm2r_text("Back to the options", "回到选项");
        N3DSGml_setGlobalString("tiptext", tip);
        return;
    }
    for (int i = 0; i < CHEAT_COUNT; i++) {
        Instance* row = N3DSAm2r_pageRow(i);
        if (row == NULL) continue;
        N3DSGml_setVarString(row, "optext", gAm2r.cheats[i] ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
        N3DSGml_setVar(row, "enabled", (i == CHEAT_MASTER || gAm2r.cheats[CHEAT_MASTER]) ? 1.0 : 0.0);
    }
    N3DSGml_setGlobalString("tiptext", selected >= 0 && selected < CHEAT_COUNT ? (N3DSAm2r_languageIs("schinese") ? kCheatTipsZh[selected] : kCheatTips[selected])
        : N3DSAm2r_text("Back to the options", "回到选项"));
}

// Pages built from the game's own row objects, where the game's pages go (oOptionsMain creates them at (50, 92)):
// Display in place of the game's Display page (Screen, Cheats, Exit), and Cheats from it.
static void N3DSAm2r_openPage(N3DSAm2rPage page, int32_t selected) {
    float x = N3DS_AM2R_PAGE_X, y = N3DS_AM2R_PAGE_Y - 8.0f;
    Instance* title = N3DSGml_create(x, y, gAm2r.objMenuLabel);
    if (title != NULL) {
        N3DSGml_setVarString(title, "text", page == PAGE_DISPLAY ? N3DSAm2r_text("Display", "显示") : N3DSAm2r_text("Cheats", "作弊"));
    }
    gAm2r.pageTitle = title != NULL ? (int32_t) title->instanceId : -1;
    int count = N3DSAm2r_pageRowCount(page);
    for (int i = 0; i < count; i++) {
        bool lrRow = page == PAGE_DISPLAY ? (i == DISPLAY_ROW_SCREEN || i == DISPLAY_ROW_FRAMESKIP || i == DISPLAY_ROW_NATIVE ||
            i == DISPLAY_ROW_FPS) : i < CHEAT_COUNT;
        const char* label = i == count - 1 ? N3DSAm2r_text("Exit", "返回")
            : page == PAGE_DISPLAY ? (i == DISPLAY_ROW_SCREEN ? N3DSAm2r_text("Screen", "画面模式")
                : i == DISPLAY_ROW_FRAMESKIP ? N3DSAm2r_text("Frameskip", "跳帧")
                : i == DISPLAY_ROW_NATIVE ? N3DSAm2r_text("Fast scripts", "快速脚本")
                : i == DISPLAY_ROW_FPS ? N3DSAm2r_text("FPS counter", "帧率显示")
                : N3DSAm2r_text("Cheats", "作弊"))
            : (N3DSAm2r_languageIs("schinese") ? kCheatLabelsZh[i] : kCheatLabels[i]);
        Instance* row = N3DSGml_create(x, y + N3DS_AM2R_ROW_SEP * (float) (i + 1), lrRow ? gAm2r.objOptionLR : gAm2r.objPauseOption);
        gAm2r.pageRows[i] = row != NULL ? (int32_t) row->instanceId : -1;
        if (row == NULL) continue;
        N3DSGml_setVar(row, "optionid", (double) i);
        N3DSGml_setVarString(row, "label", label);
    }
    N3DSGml_setGlobal("curropt", (double) selected);
    gAm2r.page = page;
    gAm2r.pageFrames = 0;
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
        // Back: from Cheats to Display, from Display to the options.
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
        N3DSAm2rPage page = gAm2r.page;
        N3DSAm2r_closePage();
        if (page == PAGE_CHEATS) N3DSAm2r_openPage(PAGE_DISPLAY, DISPLAY_ROW_CHEATS);
        else N3DSGml_create(N3DS_AM2R_PAGE_X, N3DS_AM2R_PAGE_Y, gAm2r.objOptionsMain);
        return;
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_CHEATS && menu1) {
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
        N3DSAm2r_closePage();
        N3DSAm2r_openPage(PAGE_CHEATS, 0);
        return;
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_SCREEN && (menu1 || left || right)) {
        int slot = N3DSAm2r_screenModeSlot() + (left ? N3DS_AM2R_SCREEN_MODES - 1 : 1);
        N3DS_setScreenMode(kScreenModes[slot % N3DS_AM2R_SCREEN_MODES]);
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_FRAMESKIP && (menu1 || left || right)) {
        N3DS_setFrameskip(!N3DS_getFrameskip());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_NATIVE && (menu1 || left || right) &&
               N3DSAm2rNative_available() > 0) {
        N3DSAm2rNative_setEnabled(!N3DSAm2rNative_enabled());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_FPS && (menu1 || left || right)) {
        N3DS_setFpsCounter(!N3DS_getFpsCounter());
        N3DSAm2r_sfx(gAm2r.sndMenuSel);
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

static void N3DSAm2r_applyCheats(void) {
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
    if (gAm2r.page != PAGE_NONE) {
        N3DSAm2r_updatePage();
    } else if (gAm2r.objOptionsDisplay >= 0) {
        Instance* display = N3DSGml_anyInstance(gAm2r.objOptionsDisplay);
        if (display != NULL) {
            N3DSGml_destroy(display);
            N3DSAm2r_openPage(PAGE_DISPLAY, DISPLAY_ROW_SCREEN);
        }
    }
    N3DSAm2r_applyCheats();
}
