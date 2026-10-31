#include "n3ds_am2r.h"
#include "n3ds_pause.h"
#include "n3ds_am2r_native.h"

#include "n3ds_font_replace.h"
#include "n3ds_gml.h"
#include "../text_utils.h"
#include "n3ds_platform_config.h"
#include "n3ds_renderer.h"
#include "n3ds_screen.h"

#include "../ini.h"
#include "../log.h"
#include "../renderer.h"
#include "../utils.h"
#include "../vm.h"

#include <stb/ds/stb_ds.h>
#include <math.h>
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
#define N3DS_AM2R_CREDIT_Y 190.0f
// The credit rows want the 8px Fusion Pixel strike. Every font the game replaces lands at 10px or larger
// (n3ds_font_replace.c pins those sizes) and fontMenuTiny - the slot they used to borrow - is pinned to 10px because
// the items page needed it bigger. AM2R's own GML never assigns font0 (it only sets fontGUI and friends), so font0
// is free to carry a face of its own.
#define N3DS_AM2R_CREDIT_FONT 0
#define N3DS_AM2R_CREDIT_ROW_SEP 10.0f
#define N3DS_AM2R_ROW_SEP 16.0f
#define N3DS_AM2R_SUPER_DAMAGE 5000.0
#define N3DS_AM2R_CHEATS_FILE N3DS_SD_DIR "cheats.ini"

enum { CHEAT_MASTER, CHEAT_HEALTH, CHEAT_AMMO, CHEAT_BEAM, CHEAT_BOMBS, CHEAT_MISSILES, CHEAT_COUNT };
enum {
    DISPLAY_ROW_SCREEN,
    DISPLAY_ROW_HUD,
    DISPLAY_ROW_FRAMESKIP,
    DISPLAY_ROW_NATIVE,
    DISPLAY_ROW_FPS,
    DISPLAY_ROW_CHEATS,
    DISPLAY_ROW_EXIT,
    DISPLAY_ROW_COUNT
};

typedef enum { PAGE_NONE, PAGE_DISPLAY, PAGE_CHEATS } N3DSAm2rPage;

static const char* const kCheatNames[CHEAT_COUNT] = { "master", "health", "ammo", "beam", "bombs", "missiles" };
static const char* const kCheatLabels[CHEAT_COUNT] = {
    "Cheats", "Unlimited Health", "Unlimited Ammo", "Super Beam", "Super Bombs", "Super Missiles",
};
static const char* const kCheatLabelsZh[CHEAT_COUNT] = {
    "金手指", "无限生命", "无限弹药", "强化光束", "强化炸弹", "强化导弹",
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
    "打开或关闭全部金手指",
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
    int32_t objSSFg, objSSBg;
    int32_t sndSpin[5]; // the spin-jump loops: plain, Space Jump, Screw Attack, both, underwater
    int32_t sprA, sprB, sprX, sprY;
    int32_t sfxPlay, sndMenuMove, sndMenuSel;
    int32_t roomController, roomTitle;
    char* keyboardLabel; // what get_text gave for the Keyboard settings row
    int32_t lastControlPage;
    bool cheats[CHEAT_COUNT];
    // The game's own HUD switch (the global opshowhud; the game's Options page had the row, this port's Display page
    // has it now). Only pushed onto the game once the player sets it here, so a save that carries the game's own
    // HUD setting is left alone until then.
    bool hud;
    bool hudLoaded;
    bool fontsEnabled; // diagnostic (cheats.ini's "fonts="): false keeps the game's own fonts
    // How far the game's own HUD offsets overshoot the port's window on each side. The game draws its HUD into
    // gui_surface laid out for a window as wide as its picture (426) and blits that surface with displayx, so in Wide
    // the left-hand block (health, tanks, ammo, from x = 0) is shifted off the left edge while the right-hand block
    // (map, Metroid counter) stays put. Each block is moved back by the columns its side lost.
    double hudInset;
    double hudLeftInset;
    // Ground truth for the screen geometry (see N3DSAm2r_applyScreenMode): printed on the first frame and on every
    // mode change, then every few seconds, so a screenshot can be checked against numbers instead of guessed at.
    bool screenLogged;
    int32_t screenLogMode;
    int32_t screenLogFrame;
    int32_t screenLogCount;
    // The Display or Cheats page while it is up: instance ids of its title and rows (the last one Exit).
    N3DSAm2rPage page;
    int32_t pageFrames;
    int32_t pageTitle;
    int32_t creditLines[3]; // static labels under the page: the localisation's authors, empty when unknown
    char author1[160];
    char author2[160];
    char creditText[3][200];  // exact strings of the three credit rows, so draw_text can spot them
    int32_t creditFont;       // what the credit rows are drawn with (N3DS_AM2R_CREDIT_FONT, or the menu font)
    bool creditFontReady;     // the 8px face installed into font0
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

// draw_text's original entry point, kept when the hook below replaces it by name (see N3DSAm2r_init).
static BuiltinFunc gDrawTextBuiltin = NULL;
// instance_create's original entry point, kept the same way (see hookInstanceCreate).
static BuiltinFunc gInstanceCreateBuiltin = NULL;

static RValue hookGetText(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookGetXJoyBtnName(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookGetXJoyBtnSprite(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDamageSamus(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDamageSamusKnockdown(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDamageSamusPush(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookRoomChange(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookDrawText(VMContext* ctx, RValue* args, int32_t argCount);
static RValue hookInstanceCreate(VMContext* ctx, RValue* args, int32_t argCount);

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

// draw_text(x, y, string). One string the game draws is a literal in its own bytecode, not a language-file key, so no
// translation can reach it: the pause screen's top layer draws "[Select] -> Logs" (oSubScrTopLayer's Draw event, at
// 0xd00388 in 1.5.5). It is translated here, and only when the string is exactly that one - every other draw_text in
// the game passes straight through, so the HUD, dialogue and item screens are untouched.
static RValue hookDrawText(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount == 3 && args[2].type == RVALUE_STRING && args[2].string != NULL &&
        strcmp(args[2].string, "[Select] -> Logs") == 0) {
        RValue translated = RValue_makeOwnedString(safeStrdup("[Select] -> 日志"));
        RValue a[3] = { args[0], args[1], translated };
        a[2].ownsReference = false;
        RValue result = gDrawTextBuiltin != NULL ? gDrawTextBuiltin(ctx, a, 3) : RValue_makeUndefined();
        RValue_free(&translated);
        return result;
    }
    // The credit rows are plain option rows, so they take the settings pages' font - the large one the menu is drawn
    // with. Draw them with the small font instead, restoring the game's afterwards.
    //
    // oPauseOption paints its label twice: once in black at (+1,-3) for an outline, once in its own colour at (0,-4).
    // At the 12px menu size the outline reads as a subtle edge, but these rows are drawn at 10px, where a pixel of
    // black under every glyph makes the small Chinese face look muddy, so the black pass is dropped here. That leaves
    // the coloured pass alone, which is what a plain 10px render looks like.
    if (argCount == 3 && args[2].type == RVALUE_STRING && args[2].string != NULL && gAm2r.creditFont >= 0) {
        for (int32_t i = 0; i < 3; i++) {
            if (gAm2r.creditText[i][0] == '\0' || strcmp(args[2].string, gAm2r.creditText[i]) != 0) continue;
            Renderer* renderer = ctx->runner != NULL ? ctx->runner->renderer : NULL;
            if (renderer == NULL) break;
            if (renderer->drawColor == 0x000000u) return RValue_makeUndefined(); // the outline pass: skip it
            int32_t font = renderer->drawFont;
            int32_t halign = renderer->drawHalign;
            // Centre through the renderer instead of a width measured out here. N3DSAm2r_textWidth sums the glyph
            // advances and disagrees with what draw_text actually advances by a few pixels (kerning, the trailing
            // space after the prefix), which left each of the three lines off-centre by a different amount. With
            // halign 1 the renderer puts the line's own centre on x, so one x centres all three exactly.
            //
            // x is the room's centre, which is where the game draws its own centred text (oOptionsFG puts the
            // description at 160): widescreen widens the view around that point rather than moving it, so 160 is the
            // middle of the picture in every screen mode.
            args[0] = RValue_makeReal((GMLReal) 160.0f);
            renderer->drawFont = gAm2r.creditFont;
            renderer->drawHalign = 1;
            RValue result = gDrawTextBuiltin != NULL ? gDrawTextBuiltin(ctx, args, argCount) : RValue_makeUndefined();
            renderer->drawHalign = halign;
            renderer->drawFont = font;
            return result;
        }
    }
    return gDrawTextBuiltin != NULL ? gDrawTextBuiltin(ctx, args, argCount) : RValue_makeUndefined();
}

// ===[ Water / lava ripple filter ]===
// oWaterFXV2 builds a full-screen surface in its Create event and ripples it in Step and Draw. The port has no
// equivalent for that pass - gpuSetColorWriteEnable, which it needs, is not implemented - so none of it ever reached
// the screen. It used to be handled by destroying the instance every frame (the old N3DSAm2r_dropWaterFilter): the
// game rebuilt it, the port destroyed it again, and that ran for as long as water was on screen. That cost showed up
// as the water areas' step time climbing from about 4.7 to 11 ms with the frame rate falling to 30.
//
// Refusing the creation outright is cheaper and safer than that loop: no instance, no surface, no VRAM taken, and
// nothing for the game to rebuild. The object draws nothing and nothing reads it, so skipping it changes no game
// state - the ripple effect is simply absent, which is exactly what the screen already showed.
static RValue hookInstanceCreate(VMContext* ctx, RValue* args, int32_t argCount) {
    if (argCount >= 3 && gAm2r.objWaterFX >= 0 && RValue_toInt32(args[2]) == gAm2r.objWaterFX) {
        static int32_t skipped;
        if (skipped < 3) {
            skipped++;
            logInfo("AM2R: the water ripple filter is not created (it has no 3DS equivalent)\n");
        }
        return RValue_makeReal((GMLReal) INSTANCE_NOONE);
    }
    return gInstanceCreateBuiltin != NULL ? gInstanceCreateBuiltin(ctx, args, argCount) : RValue_makeUndefined();
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

// Declared here because the main menu's own hint below needs it before the front end's helpers are defined.
static const char* N3DSAm2r_text(const char* english, const char* chinese);

static RValue hookGetText(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    const char* section = argCount > 0 && args[0].type == RVALUE_STRING ? args[0].string : "";
    const char* key = argCount > 1 && args[1].type == RVALUE_STRING ? args[1].string : "";
    // The main menu's hint for the Display entry. The port adds that entry (the game has no screen-mode page), so its
    // hint is the port's own text and has to be translated like the rest of the front end.
    if (strcmp(section, "OptionsMain") == 0 && strcmp(key, "Display_Tip") == 0) {
        return RValue_makeOwnedString(safeStrdup(N3DSAm2r_text("Screen mode and cheats", "画面模式与金手指")));
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
    if (!gAm2r.fontsEnabled) return RValue_makeUndefined(); // diagnostic: leave the game's own fonts in place
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
    // The game hands over a program_directory path ("lang/titles/<file>.png"), which does not open as it is: the 3DS
    // runs with a current directory that is not the game's folder (see N3DSFontReplace_resolvePath).
    char* fullPath = N3DSFontReplace_resolvePath(args[1].string);
    stbi_uc* pixels = fullPath != NULL ? stbi_load(fullPath, &width, &height, &channels, 4) : NULL;
    free(fullPath);
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

// The screen modes the Display page offers, in order (Wide first: the default).
static const N3DSScreenMode kScreenModes[] = { N3DS_SCREEN_WIDE, N3DS_SCREEN_STRETCH, N3DS_SCREEN_1X };
// The modes' names as the saved file and the log use them: kept in English and never translated, because the save
// (cheats.ini's "screen=Wide") is read back by name - a translated one would stop an old save being understood. The
// names the player sees are N3DSAm2r_screenModeLabel, below (they are translated).
static const char* const kScreenModeNames[] = { "Wide", "Stretch", "Point" };
#define N3DS_AM2R_SCREEN_MODES ((int) (sizeof(kScreenModes) / sizeof(kScreenModes[0])))

static int N3DSAm2r_screenModeSlot(void) {
    N3DSScreenMode mode = N3DS_getScreenMode();
    for (int i = 0; i < N3DS_AM2R_SCREEN_MODES; i++) {
        if (kScreenModes[i] == mode) return i;
    }
    return 0;
}

// The mode's name for the log (the menu's names are the slots' names, so a mode the build doesn't offer logs "?").
static const char* N3DSAm2r_screenModeName(N3DSScreenMode mode) {
    for (int i = 0; i < N3DS_AM2R_SCREEN_MODES; i++) {
        if (kScreenModes[i] == mode) return kScreenModeNames[i];
    }
    return "?";
}

// The saved value is the mode's name, not its slot: the modes were reordered (and one removed) after the first builds,
// so an old "screen=<slot>" would silently mean a different mode. A name the build no longer offers is ignored and the
// default stands.
static bool N3DSAm2r_screenModeFromName(const char* name, N3DSScreenMode* out) {
    for (int i = 0; i < N3DS_AM2R_SCREEN_MODES; i++) {
        if (strcmp(name, kScreenModeNames[i]) == 0) {
            *out = kScreenModes[i];
            return true;
        }
    }
    return false;
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

// The screen mode's name as the Display page shows it. The saved name (kScreenModeNames) stays English - see the note
// on it - so the player-facing name is separate and translated here. An unknown mode falls back to the saved name.
static const char* N3DSAm2r_screenModeLabel(N3DSScreenMode mode) {
    switch (mode) {
        case N3DS_SCREEN_WIDE: return N3DSAm2r_text("Wide", "宽屏");
        case N3DS_SCREEN_STRETCH: return N3DSAm2r_text("Stretch", "拉伸");
        case N3DS_SCREEN_1X: return N3DSAm2r_text("Point", "点对点");
        default: return N3DSAm2r_screenModeName(mode);
    }
}

// Every Chinese character the front end draws. They are in no language file, so without this they would be missing
// from the atlas the loader builds and would draw as blanks. Call before oControl's first font_replace.
static void N3DSAm2r_registerPortGlyphs(void) {
    // These must be the same strings the pages draw (the kCheat*Zh tables above and the N3DSAm2r_text calls in
    // N3DSAm2r_refreshPage and N3DSAm2r_openPage): a character that only appears here is wasted atlas space, and one
    // that is missing renders as nothing at all.
    static const char* const kPortChinese[] = {
        // Titles, row labels and row values.
        // The bottom screen's own two buttons - they are drawn by this port, so nothing else registers them.
        "萨姆斯", "地图", "画面模式与金手指",
        "显示", "金手指", "返回", "画面模式", "状态栏", "跳帧", "快速脚本", "帧率显示", "开", "关", "不可用",
        "回到选项", "此版本的游戏不支持", "左下角的小型帧率计数器", "无限生命和弹药，更强的武器",
        // The Display page's tips.
        "宽屏填满屏幕；拉伸压缩填满；点对点原生 320x240",
        "游戏中显示的能量、弹药与地图",
        "保持完整游戏速度下稳定的 30 帧（隔帧绘制）",
        "繁重的游戏脚本改用内置代码运行：更快，行为相同",
        // The Cheats page's rows and tips.
        "无限生命", "无限弹药", "强化光束", "强化炸弹", "强化导弹",
        "打开或关闭全部金手指", "能量始终保持全满",
        "3DS 移植：B站 等璃籽", "译者：",
        "导弹、超级导弹与强力炸弹始终保持全满",
        "光束击中任何目标都会将其摧毁",
        "炸弹与强力炸弹击中任何目标都会将其摧毁",
        "导弹击中任何目标都会将其摧毁，对 Metroid 也有效",
        // The screen modes' player-facing names (N3DSAm2r_screenModeLabel; "Point" is 点对点 in either language).
        "宽屏", "拉伸",
        // The pause screen's "[Select] -> Logs" hint, which the game draws from its own bytecode (hookDrawText).
        "[Select] -> 日志",
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
        char text[32];
        int value = 0;
        // The value is read as text as well as a number: "screen" is a mode name (see N3DSAm2r_screenModeFromName).
        if (sscanf(line, "%31[^=]=%31s", name, text) != 2) continue;
        value = atoi(text) != 0;
        if (strcmp(name, "screen") == 0) {
            // Only the name. Older builds saved a slot number and the slots have since been reordered (Stretch was 0
            // then, Wide is 0 now), so a number would name the wrong mode; it is ignored and the default stands.
            N3DSScreenMode mode = N3DS_SCREEN_WIDE;
            if (N3DSAm2r_screenModeFromName(text, &mode)) N3DS_setScreenMode(mode);
        }
        if (strcmp(name, "frameskip") == 0) N3DS_setFrameskip(value != 0);
        if (strcmp(name, "native") == 0) N3DSAm2rNative_setEnabled(value != 0);
        if (strcmp(name, "fps") == 0) N3DS_setFpsCounter(value != 0);
        // A diagnostic switch: fonts=0 keeps the game's own fonts, so a text problem can be split in half - the
        // swapped-in font and its texture, or the renderer drawing it.
        if (strcmp(name, "fonts") == 0) gAm2r.fontsEnabled = value != 0;
        if (strcmp(name, "hud") == 0) {
            gAm2r.hud = value != 0;
            gAm2r.hudLoaded = true;
        }
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
    // The name, not the slot: the modes were reordered after the first builds, so a slot would not mean the same mode
    // to an older or newer build (see N3DSAm2r_loadCheats).
    fprintf(f, "screen=%s\n", kScreenModeNames[N3DSAm2r_screenModeSlot()]);
    fprintf(f, "frameskip=%d\n", N3DS_getFrameskip() ? 1 : 0);
    fprintf(f, "native=%d\n", N3DSAm2rNative_enabled() ? 1 : 0);
    fprintf(f, "fps=%d\n", N3DS_getFpsCounter() ? 1 : 0);
    // Only once the player has set it: writing the game's own value here would make a choice the player never made here
    // look like one (and would then be pushed onto the game).
    if (gAm2r.hudLoaded) fprintf(f, "hud=%d\n", gAm2r.hud ? 1 : 0);
    fclose(f);
}

static bool N3DSAm2r_cheat(int which) {
    return gAm2r.cheats[CHEAT_MASTER] && gAm2r.cheats[which];
}

// ===[ The game's own HUD switch ]===

// The game draws its HUD from the global opshowhud (and its own Options page had the row for it, which this port
// replaced with its Display page). The port keeps the choice and states it to the game every frame: the game's own
// option loader (load_gameoptions, called when a save is loaded) would put its saved value back otherwise. Until the
// player sets it here, the game's own value is left as it is.
void N3DSAm2r_setHudEnabled(bool on) {
    gAm2r.hud = on;
    gAm2r.hudLoaded = true;
    N3DSGml_setGlobal("opshowhud", on ? 1.0 : 0.0);
}

bool N3DSAm2r_hudEnabled(void) {
    if (gAm2r.hudLoaded) return gAm2r.hud;
    // The player has not set it here: what the game itself has (the game's own Options page had the row, and a save
    // carries its value). Nothing to show as the row's state means the game's default, on.
    double shown = 1.0;
    if (N3DSGml_getGlobal("opshowhud", -1, &shown)) return shown > 0.5;
    return true;
}

static void N3DSAm2r_applyHud(void) {
    if (!gAm2r.hudLoaded) return;
    double shown = 0.0;
    if (!N3DSGml_getGlobal("opshowhud", -1, &shown)) return; // the global is not there yet: nothing to state it to
    if ((shown > 0.5) != gAm2r.hud) N3DSGml_setGlobal("opshowhud", gAm2r.hud ? 1.0 : 0.0);
}

// ===[ Init ]===

static void N3DSAm2r_loadAuthors(void);

// The credits are small text under the settings menu. They used to borrow fontMenuTiny, which the port pins to 10px
// for the items page, so they came out at 10px; the 8px Fusion Pixel strike reads closer to the menu's own type and
// keeps the longest line (four names) inside the picture. The glyph set is the same one every other font uses.
static void N3DSAm2r_installCreditFont(Runner* runner) {
    if (runner == NULL) return;
    gAm2r.creditFontReady = N3DSFontReplace_apply(
        runner, N3DS_AM2R_CREDIT_FONT, "lang/fonts/04b24.ttf", 8.0f, false, false);
    if (!gAm2r.creditFontReady) {
        logWarn("AM2R: no 8px credit face; the credit rows fall back to the menu font\n");
    }
}

void N3DSAm2r_init(Runner* runner) {
    memset(&gAm2r, 0, sizeof(gAm2r));
    gAm2r.runner = runner;
    gAm2r.fontsEnabled = true; // cheats.ini's "fonts=0" turns the font swapping off (a diagnostic)
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
    // event, which is where the language pack calls them. draw_text is a plain builtin too, and is wrapped the same
    // way (see hookDrawText: one pause-screen string the game draws from a literal, which no language file holds).
    for (uint32_t i = 0; i < vm->funcCallCacheCount; i++) {
        const char* name = dataWin->func.functions[i].name;
        if (name == NULL) continue;
        if (strcmp(name, "font_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookFontReplace;
            logInfo("AM2R: font_replace goes through the 3DS front end\n");
        } else if (strcmp(name, "background_replace") == 0) {
            vm->funcCallCache[i].builtin = (void*) hookBackgroundReplace;
            logInfo("AM2R: background_replace goes through the 3DS front end\n");
        } else if (strcmp(name, "draw_text") == 0) {
            gDrawTextBuiltin = vm->funcCallCache[i].builtin;
            vm->funcCallCache[i].builtin = (void*) hookDrawText;
        } else if (strcmp(name, "instance_create") == 0) {
            gInstanceCreateBuiltin = vm->funcCallCache[i].builtin;
            vm->funcCallCache[i].builtin = (void*) hookInstanceCreate;
        }
    }
    // Fast scripts default to on; cheats.ini can turn them off.
    N3DSAm2rNative_setEnabled(true);
    N3DSAm2r_loadCheats();
    N3DSAm2r_loadLanguages();
    N3DSAm2r_loadAuthors();
    // The 3DS front end's own pages are not in any language file, so their characters have to be offered to the font
    // loader before oControl builds the atlas, or they render as blanks.
    N3DSAm2r_registerPortGlyphs();
    // The on-card font cache has to come back here: after the port's own characters have been offered to the glyph set
    // (the cache key includes it, and a set built earlier is missing them) and before the credit font below installs a
    // face, which would otherwise take its glyph rectangles from an atlas the cache then overwrites.
    N3DSFontReplace_loadCache(runner);
    N3DSAm2r_installCreditFont(runner);
    N3DSAm2rNative_init(runner);
}

// The language file's [Header] Author / Author2: the game itself only reads "Language" from that section, so on the
// PC build these appear under the Display page's language row, and here they are shown below the front end's own rows.
static void N3DSAm2r_loadAuthors(void) {
    gAm2r.author1[0] = '\0';
    gAm2r.author2[0] = '\0';
    int32_t slot = N3DSAm2r_languageSlot();
    if (slot < 0 || slot >= gLanguageCount) return;
    char path[512];
    snprintf(path, sizeof(path), N3DS_SD_DIR "lang/%s", gLanguages[slot].file);
    char author[160];
    N3DSAm2r_readIniString(path, "Header", "Author", author, sizeof(author));
    if (author[0] != '\0') snprintf(gAm2r.author1, sizeof(gAm2r.author1), "%s", author);
    N3DSAm2r_readIniString(path, "Header", "Author2", author, sizeof(author));
    if (author[0] != '\0') snprintf(gAm2r.author2, sizeof(gAm2r.author2), "%s", author);
    if (gAm2r.author1[0] != '\0') logInfo("AM2R: localisation authors: %s / %s\n", gAm2r.author1, gAm2r.author2);
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
        if (screen != NULL) N3DSGml_setVarString(screen, "optext", N3DSAm2r_screenModeLabel(N3DS_getScreenMode()));
        Instance* hud = N3DSAm2r_pageRow(DISPLAY_ROW_HUD);
        if (hud != NULL) N3DSGml_setVarString(hud, "optext", N3DSAm2r_hudEnabled() ? N3DSAm2r_text("ON", "开") : N3DSAm2r_text("OFF", "关"));
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
                ? N3DSAm2r_text("Wide fills the screen; Stretch squashes; Point is the native 320x240",
                    "宽屏填满屏幕；拉伸压缩填满；点对点原生 320x240")
            : selected == DISPLAY_ROW_HUD
                ? N3DSAm2r_text("The energy, ammo and map display during play", "游戏中显示的能量、弹药与地图")
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
        N3DSGml_setVarString(title, "text", page == PAGE_DISPLAY ? N3DSAm2r_text("Display", "显示") : N3DSAm2r_text("Cheats", "金手指"));
    }
    gAm2r.pageTitle = title != NULL ? (int32_t) title->instanceId : -1;
    int count = N3DSAm2r_pageRowCount(page);
    for (int i = 0; i < count; i++) {
        bool lrRow = page == PAGE_DISPLAY ? (i == DISPLAY_ROW_SCREEN || i == DISPLAY_ROW_HUD || i == DISPLAY_ROW_FRAMESKIP ||
            i == DISPLAY_ROW_NATIVE || i == DISPLAY_ROW_FPS) : i < CHEAT_COUNT;
        const char* label = i == count - 1 ? N3DSAm2r_text("Exit", "返回")
            : page == PAGE_DISPLAY ? (i == DISPLAY_ROW_SCREEN ? N3DSAm2r_text("Screen", "画面模式")
                : i == DISPLAY_ROW_HUD ? N3DSAm2r_text("HUD", "状态栏")
                : i == DISPLAY_ROW_FRAMESKIP ? N3DSAm2r_text("Frameskip", "跳帧")
                : i == DISPLAY_ROW_NATIVE ? N3DSAm2r_text("Fast scripts", "快速脚本")
                : i == DISPLAY_ROW_FPS ? N3DSAm2r_text("FPS counter", "帧率显示")
                : N3DSAm2r_text("Cheats", "金手指"))
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
    } else if (gAm2r.page == PAGE_DISPLAY && selected == DISPLAY_ROW_HUD && (menu1 || left || right)) {
        N3DSAm2r_setHudEnabled(!N3DSAm2r_hudEnabled());
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
// Handled by refusing the object's creation outright - see hookInstanceCreate, next to hookDrawText. This used to
// destroy the instance every frame instead, which had the game rebuild it every frame for as long as water was on
// screen; that is what made the water areas slow.

int32_t N3DSAm2r_gameWidth(void) {
    if (!gAm2r.active) return 0;
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    double widescreen = 0.0, space = 0.0;
    if (control == NULL || !N3DSGml_getVar(control, "widescreen", &widescreen) || widescreen < 0.5) return 0;
    if (!N3DSGml_getVar(control, "widescreen_space", &space) || space <= 0.0) return 0;
    return 320 + (int32_t) space;
}

// AM2R 1.5's own widescreen is 320 + widescreen_space wide, and the game's default space (106) makes a 426x240 (16:9)
// picture. The top screen is 400x240 (5:3), so 426 columns do not all fit at 1:1 and something has to give.
//
// The picture width is read from the surface itself rather than assumed: the game keeps its own 426-wide picture (the
// port does not clamp widescreen_space any more), and reading the surface means the transform stays right whatever
// width the game's own widescreen ends up at.
//
// The game lays its picture out itself, in the window it is told about. Its own Step reads window_get_width()/height()
// (the port returns gWindowW/gWindowH, see main.c) and window_get_fullscreen(), which this port stubs to 0, so AM2R
// always takes its window branch and never its display branch:
//
//   display_scale = window_get_width() / (320 + widescreen_space)     [when window H < window W]
//   display_scale = window_get_height() / 240                         [otherwise]
//   displayx      = window_get_width()  / 2 - (320 + space) * scale / 2
//   displayy      = window_get_height() / 2 - 240 * scale / 2
//
// In Wide and 1x the window is the 400x240 screen, so both branches give scale 1.0 and displayx -13: the game fits the
// 426-wide picture by scaling it down (0.939) and centres it, which is exactly the same geometry as Wide. That is why
// 1x and Wide used to look identical - it is not a coincidence, the game cannot produce a 1x fit at a 400-wide window.
// So for those two modes the port states the transform itself instead of hoping the game picks it:
//
//   Wide: scale 1        the picture 1:1, filling the screen, the 26 columns past 400 given up 13/13, no bars
//   1x:   scale 400/426  the whole picture fitted (400x225), centred, with ~7px bars top and bottom
//
// Stretch and 2x leave the game's own values alone: they give it a window as wide as its picture, so its own 1:1 layout
// is already what those modes want before the port transforms it.
#define N3DS_AM2R_WIDE_SCREEN_W 400
// The game's picture is 320 + widescreen_space wide. Its own 106 makes a 426x240 (16:9) picture, which the 400-wide top
// screen can only show by giving up 13 columns each side. At 80 the picture is exactly 400x240: every column the game
// draws is a column on the screen, with nothing cropped and nothing scaled.
#define N3DS_AM2R_NATIVE_SPACE 80.0
// The game's default widescreen space is 106 (its 426-wide picture is 320 + it). It is NOT a HUD inset: the HUD is
// laid out at 250 + the space the game currently reports, so the inset has to be that live value - see the
// point-for-point branch in N3DSAm2r_applyScreenMode.

// Where the picture has to start to be centred on the screen: negative (the overflow split evenly 13/13 for the 426-wide
// picture) when it is wider than the screen, positive (the bars split evenly) when it is narrower.
static double N3DSAm2r_centredDisplayX(double pictureW, double scale) {
    return floor(((double) N3DS_AM2R_WIDE_SCREEN_W - pictureW * scale) * 0.5);
}

// The HUD (the game's draw_gui) is drawn into gui_surface, laid out for a window as wide as the game's own picture, and
// the DrawGUI event then blits that whole surface with the picture's transform (displayx, display_scale). So the columns
// the picture loses come off the HUD's matching side too: in Wide the left-hand block (health, tanks, ammo, laid out
// from x = 0) is pushed off the left edge, and the right-hand block (map, Metroid counter, laid out at 250/272/276/296 +
// widescreen_space) reaches past the right one. Reporting each side's loss lets the native draw_gui move both blocks
// back in, so each keeps the margin the game gave it.
// The columns the port adds to the game's 320 to make the picture. In Wide that is N3DS_AM2R_NATIVE_SPACE, stated by
// this front end rather than read from the game: the game's own 106 makes a 426-wide picture the screen cannot show.
// The HUD is laid out with this, not with the game's variable - draw_gui runs inside the game's Step, a step before
// the port restates the value for the views, so the variable is a step stale where the HUD is concerned.
double N3DSAm2r_pictureSpace(void) {
    return N3DS_getScreenMode() == N3DS_SCREEN_WIDE ? N3DS_AM2R_NATIVE_SPACE : 0.0;
}

double N3DSAm2r_hudRightInset(void) {
    return gAm2r.hudInset;
}

double N3DSAm2r_hudLeftInset(void) {
    return gAm2r.hudLeftInset;
}

// Ground truth, read back out of the instance after the writes: the numbers a screenshot has to agree with. Printed on
// the first frame and on every mode change, then a few times more, so one run on hardware tells a wrong variable from a
// wrong blit.
static void N3DSAm2r_logScreenGeometry(Instance* control, double pictureW, N3DSScreenMode mode) {
    int32_t frame = gAm2r.runner != NULL ? gAm2r.runner->frameCount : 0;
    bool changed = !gAm2r.screenLogged || gAm2r.screenLogMode != (int32_t) mode;
    if (!changed && !(gAm2r.screenLogCount < 10 && frame - gAm2r.screenLogFrame >= 600)) return;
    gAm2r.screenLogged = true;
    gAm2r.screenLogMode = (int32_t) mode;
    gAm2r.screenLogFrame = frame;
    gAm2r.screenLogCount++;
    double scale = 0.0, x = 0.0, y = 0.0;
    N3DSGml_getVar(control, "display_scale", &scale);
    N3DSGml_getVar(control, "displayx", &x);
    N3DSGml_getVar(control, "displayy", &y);
    // What that means on the 400x240 screen: the rectangle the picture covers and how much falls outside each edge.
    int32_t ix = (int32_t) x;
    int32_t iy = (int32_t) y;
    int32_t drawnW = (int32_t) (pictureW * scale + 0.5);
    int32_t drawnH = (int32_t) (240.0 * scale + 0.5);
    int32_t cutLeft = ix < 0 ? -ix : 0;
    int32_t cutRight = ix + drawnW > N3DS_AM2R_WIDE_SCREEN_W ? ix + drawnW - N3DS_AM2R_WIDE_SCREEN_W : 0;
    int32_t barTop = iy > 0 ? iy : 0;
    int32_t barBottom = iy + drawnH < 240 ? 240 - (iy + drawnH) : 0;
    Runner* runner = gAm2r.runner;
    logInfo("AM2R: screen %d [%s] picture %.0fx240 scale %.4f displayx/y (%d,%d): drawn %dx%d at (%d,%d), "
            "cut left %d right %d, bars top %d bottom %d (guicanvas %dx%d, hud left %.0f right %.0f)\n",
        (int) mode, N3DSAm2r_screenModeName(mode), pictureW, scale, ix, iy, drawnW, drawnH, ix, iy,
        cutLeft, cutRight, barTop, barBottom,
        runner != NULL ? runner->guiWidth : 0, runner != NULL ? runner->guiHeight : 0,
        gAm2r.hudLeftInset, gAm2r.hudInset);
}

static void N3DSAm2r_applyScreenMode(void) {
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    if (control == NULL) {
        gAm2r.hudInset = 0.0;
        gAm2r.hudLeftInset = 0.0;
        return;
    }
    // Point-for-point wants the game's own 4:3 picture, not its widescreen one. Turning the game's widescreen off is
    // the way to get it: the game then lays out a plain 320x240 picture, which fits the 400-wide top screen at 1:1 with
    // 40 columns of bar either side - no scaling, so no blur, and nothing cropped. Set here because the game rewrites
    // the flag itself (its own options page has a widescreen toggle), so it has to be restated every frame. The other
    // modes leave it alone and get the game's widescreen picture as before.
    N3DSScreenMode mode = N3DS_getScreenMode();
    // Point-for-point and Stretch both start from the game's own 4:3 picture. For point-for-point that is the whole
    // point; for Stretch it means the 320x240 picture is scaled 1.25x across rather than the 426x240 one being scaled
    // 0.94x down, which resamples less and keeps the pixels cleaner (see the filter note in n3ds_renderer.c).
    if (mode == N3DS_SCREEN_1X || mode == N3DS_SCREEN_STRETCH) {
        double flag = 0.0;
        if (N3DSGml_getVar(control, "widescreen", &flag) && flag != 0.0) {
            N3DSGml_setVar(control, "widescreen", 0.0);
        }
        // widescreen_space has to go with it: the pause pages, the log page and the HUD all lay themselves out as
        // "250 + widescreen_space" or "148 + widescreen_space". Leaving it at 80 draws a 400-wide layout onto a
        // 320-wide picture, which is what pushed the energy readout, the "scanning" note and the log text off screen.
        double space = 0.0;
        if (N3DSGml_getVar(control, "widescreen_space", &space) && space != 0.0) {
            N3DSGml_setVar(control, "widescreen_space", 0.0);
        }
        // The picture is the game's own 320x240 and it is placed by displayx, not by the renderer's viewport: the
        // picture's own drawing goes through the surface path, which sets the viewport to the whole target, so a
        // centred viewport never reaches it. 40 is the centring on a 400-wide screen: (400-320)/2.
        //
        // The HUD's right-hand block is zeroed in the draw itself for these two modes (see the map/Metroid frames in
        // n3ds_am2r_native_hud.inc): the picture is the game's plain 320x240, so the block belongs at 250 and ends at
        // 320 whatever widescreen_space happens to hold. Reading that variable here used to decide it, but the game
        // rewrites it in its own Step, so the value sampled here disagreed with the one the layout used.
        //
        // Kept at 0 so the geometry log agrees with what is actually drawn.
        gAm2r.hudInset = 0.0;
        gAm2r.hudLeftInset = 0.0;
        // The GUI canvas is widened to the whole window in the renderer's beginGUI: the game lays its HUD out at
        // displayx inside GUI space, so a canvas only as wide as the picture would clip its right-hand block. Setting
        // runner->guiWidth here does not work - the game rewrites it from its own display_set_gui_size call.
        double wantedX = mode == N3DS_SCREEN_STRETCH ? 0.0 : 40.0;
        double value = 0.0;
        if (N3DSGml_getVar(control, "display_scale", &value) && value != 1.0) {
            N3DSGml_setVar(control, "display_scale", 1.0);
        }
        if (N3DSGml_getVar(control, "displayx", &value) && value != wantedX) {
            N3DSGml_setVar(control, "displayx", wantedX);
        }
        if (N3DSGml_getVar(control, "displayy", &value) && value != 0.0) {
            N3DSGml_setVar(control, "displayy", 0.0);
        }
        // The picture is centred at displayx (40 in point-for-point), but the HUD is laid out on the whole GUI canvas
        // from x=0: without this it sat 40 columns to the left of the picture and its left end - the energy readout -
        // ran against the canvas edge. Shifting the left block by the same amount puts the HUD on the picture again.
        // 13 rather than the full displayx: the HUD only needs the crop compensation (426 -> 400 picture), not the
        // whole centring offset - the picture's own drawing already carries displayx.
        gAm2r.hudLeftInset = 13.0;
        N3DSAm2r_logScreenGeometry(control, 320.0, mode);
        return;
    }
    // Every other mode wants the game's widescreen picture, so the flag the point-for-point branch cleared is put back
    // first. Without this, switching from point-for-point to Wide or Stretch left the game 320 wide and the branches
    // below returned early, which is what broke both of those modes after this change.
    double widescreen = 0.0;
    if (N3DSGml_getVar(control, "widescreen", &widescreen) && widescreen < 0.5) {
        N3DSGml_setVar(control, "widescreen", 1.0);
        widescreen = 1.0;
    }
    // Native fullscreen: the picture is 320 + widescreen_space wide, and 80 makes exactly the screen's 400. The game
    // writes its own 106 whenever a room is entered (oGotoRoom.Step), so it is restated every frame like the flag
    // above. The widescreen surface is resized to match by the game, through checkWidescreenSurface below.
    if (mode == N3DS_SCREEN_WIDE) {
        double space = 0.0;
        if (N3DSGml_getVar(control, "widescreen_space", &space) && space != N3DS_AM2R_NATIVE_SPACE) {
            N3DSGml_setVar(control, "widescreen_space", N3DS_AM2R_NATIVE_SPACE);
        }
    }
    double space = 0.0;
    if (widescreen < 0.5) {
        gAm2r.hudInset = 0.0;
        gAm2r.hudLeftInset = 0.0;
        return;
    }
    if (!N3DSGml_getVar(control, "widescreen_space", &space) || space <= 0.0) {
        gAm2r.hudInset = 0.0; // widescreen off: the game is 320 wide on its own
        gAm2r.hudLeftInset = 0.0;
        return;
    }

    // The picture's real width. widescreen_space is deliberately NOT clamped any more: the game rewrites it from its
    // own logic every Step, so clamping it fights the game and (via checkWidescreenSurface) makes the game's surface -
    // which it sizes from its own space - be freed and rebuilt every frame. Instead the game keeps its 426-wide picture
    // and this only says where and how big to show it.
    double pictureW = (double) (320.0 + space);
    double surface = -1.0;
    Renderer* renderer = gAm2r.runner != NULL ? gAm2r.runner->renderer : NULL;
    if (renderer != NULL && N3DSGml_getVar(control, "widescreen_surface", &surface) && surface >= 0.0) {
        double width = (double) Renderer_getSurfaceWidth(renderer, (int32_t) surface);
        if (width > 0.0) pictureW = width;
    }

    // The window the game is given: the screen (400) in Wide, its own picture (426) in Stretch. Wide can leave the
    // HUD's offsets overshooting, and it states the picture's transform below.
    // Both HUD insets are the columns each side of the picture loses. They are set below, once the picture's real
    // placement is known (from the displayx/display_scale that are written there); Stretch gives the picture its own
    // width, so nothing is lost and they stay 0.
    gAm2r.hudInset = 0.0;
    gAm2r.hudLeftInset = 0.0;

    // The scale and offset the picture is drawn with. The game's own values are right for Stretch (it is given a
    // window as wide as its picture), so that mode leaves them alone; Wide states them, because at a 400-wide window
    // the game would scale its 426-wide picture down instead of filling the screen with it.
    bool stated = false;
    double wantedScale = 0.0;
    if (mode == N3DS_SCREEN_WIDE) {
        wantedScale = 1.0;
        stated = true;
    }
    if (stated) {
        // The picture is truly centred only if the integer column it starts at is derived from the scaled width; the
        // same for displayy, so the bars come out even top and bottom (1x: 400x225 at y 7, 7 above and 8 below).
        double wantedX = N3DSAm2r_centredDisplayX(pictureW, wantedScale);
        double wantedY = floor((240.0 - 240.0 * wantedScale) * 0.5);
        // The game's HUD is laid out in the picture's own coordinates and shares the picture's transform, so each side of
        // it loses exactly the columns that side of the picture loses. Moving each block back in by its own side's crop
        // is what gives the two blocks the same margin (Wide: 13 each; 1x: none - its picture starts at 0 and ends at
        // 400). Using the whole overflow for the right block instead would leave it a crop's worth short of the edge.
        // The game lays the HUD out for its own picture (320 + widescreen_space, 106 -> 426). The port makes that
        // picture 400 wide on the screen, so the left block still starts 13 columns to the left of where the game puts
        // it and the leading digit of the energy readout is cut ("99" showed only its right 9). 13 is (426 - 400) / 2,
        // the same crop the game's picture loses on each side; the right block already gets its half from rightCrop.
        double gamePictureW = 320.0 + 106.0;
        double leftCrop = (gamePictureW - (double) N3DS_AM2R_WIDE_SCREEN_W) * 0.5;
        gAm2r.hudLeftInset = (wantedX < 0.0 ? -wantedX : 0.0) + (leftCrop > 0.0 ? leftCrop : 0.0);
        double rightCrop = wantedX + pictureW * wantedScale - (double) N3DS_AM2R_WIDE_SCREEN_W;
        gAm2r.hudInset = rightCrop > 0.0 ? rightCrop : 0.0;
        double value = 0.0;
        if (N3DSGml_getVar(control, "display_scale", &value) && value != wantedScale) {
            N3DSGml_setVar(control, "display_scale", wantedScale);
        }
        if (N3DSGml_getVar(control, "displayx", &value) && value != wantedX) {
            N3DSGml_setVar(control, "displayx", wantedX);
        }
        if (N3DSGml_getVar(control, "displayy", &value) && value != wantedY) {
            N3DSGml_setVar(control, "displayy", wantedY);
        }
    }

    N3DSAm2r_logScreenGeometry(control, pictureW, mode);
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

// Width of a mixed Latin/CJK string in a font, for centring the credit lines: an option row draws its label from its
// own x, so the x has to be chosen to centre the text.
static float N3DSAm2r_textWidth(int32_t fontIndex, const char* text) {
    DataWin* dw = gAm2r.runner != NULL ? gAm2r.runner->dataWin : NULL;
    if (dw == NULL || fontIndex < 0 || (uint32_t) fontIndex >= dw->font.count) return 0.0f;
    Font* font = &dw->font.fonts[fontIndex];
    int32_t len = (int32_t) strlen(text);
    float width = 0.0f;
    int32_t pos = 0;
    while (pos < len) {
        uint16_t cp = TextUtils_decodeUtf8(text, len, &pos);
        const FontGlyph* glyph = TextUtils_findGlyph(font, cp);
        width += glyph != NULL && glyph->shift > 0 ? (float) glyph->shift : 8.0f;
    }
    return width;
}

// The localisation's authors, under the main settings menu. They come from the language file's [Header]
// Author/Author2, which the game only reads "Language" from, plus the 3DS port itself.
//
// Plain option rows rather than page titles: a title is drawn large, white, on the green highlight bar, while an
// unselected row is the small grey the settings pages use for rows that are not acting.
static void N3DSAm2r_updateCredits(void) {
    Instance* main = N3DSGml_firstInstance(gAm2r.objOptionsMain);
    bool show = main != NULL && gAm2r.page == PAGE_NONE;
    if (!show) {
        for (int32_t i = 0; i < 3; i++) {
            if (gAm2r.creditLines[i] >= 0) N3DSGml_destroy(N3DSGml_instanceById(gAm2r.creditLines[i]));
            gAm2r.creditLines[i] = -1;
        }
        return;
    }
    if (gAm2r.creditLines[0] >= 0) return; // already up
    int32_t font = gAm2r.creditFontReady ? N3DS_AM2R_CREDIT_FONT : N3DSGml_fontIndex("fontMenuTinyDefault");
    if (font < 0) font = N3DSGml_fontIndex("fontSubScrDefault");
    gAm2r.creditFont = font;
    char portLine[96];
    snprintf(portLine, sizeof(portLine), "%sB站 等璃籽", N3DSAm2r_text("3DS port: ", "3DS 移植："));
    const char* lines[3] = { gAm2r.author1, gAm2r.author2, portLine };
    for (int32_t i = 0; i < 3; i++) {
        if (lines[i][0] == '\0') continue;
        char text[200];
        // No leading spaces on the second and third lines: centring measures the whole line, spaces included, so the
        // visible text of those two sat off the middle by half the width of the indent.
        snprintf(text, sizeof(text), "%s%s", i == 0 ? N3DSAm2r_text("Translators: ", "译者：") : "", lines[i]);
        // The x here is only where the instance sits; the actual centring happens in hookDrawText, which draws these
        // lines with halign 1 at the room's centre. A width measured here would be a few pixels off from the width
        // draw_text actually advances, and each line came out centred on a slightly different point.
        float x = 0.0f;
        float rowSep = gAm2r.creditFontReady ? N3DS_AM2R_CREDIT_ROW_SEP : 12.0f;
        Instance* line = N3DSGml_create(x, N3DS_AM2R_CREDIT_Y + rowSep * (float) i, gAm2r.objPauseOption);
        if (line == NULL) continue;
        N3DSGml_setVarString(line, "label", text);
        // A row draws at half alpha when it is not enabled - the grey the settings pages use for rows that cannot act.
        N3DSGml_setVar(line, "enabled", 0.0);
        snprintf(gAm2r.creditText[i], sizeof(gAm2r.creditText[i]), "%s", text);
        gAm2r.creditLines[i] = (int32_t) line->instanceId;
    }
}

// The pause room's pages size their text box with oControl.widescreen_space (149 + space) and wrap the text with the
// same value (148 + space). The game resets it on room entry, so a page built before the port restates it gets a 149-wide
// box while the text wraps at 228 - the right of the screen stays black and the text is clipped. Writing it here, before
// the room's Create events run, makes both halves agree.
void N3DSAm2r_prepareRoomChange(void) {
    if (!gAm2r.active || N3DS_getScreenMode() != N3DS_SCREEN_WIDE) return;
    Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
    if (control == NULL) return;
    double space = 0.0;
    if (N3DSGml_getVar(control, "widescreen_space", &space) && space != N3DS_AM2R_NATIVE_SPACE) {
        N3DSGml_setVar(control, "widescreen_space", N3DS_AM2R_NATIVE_SPACE);
    }
}

void N3DSAm2r_update(void) {
    if (!gAm2r.active) return;
    // Widescreen belongs to the game's picture. The pause room (map, items, log pages) and the HUD are laid out for the
    // game's own 320-wide canvas; handing them the widescreen space drew a 400-wide layout that ran off the right of the
    // screen - the energy readout cut to "E - 00", the "scanning" note past the edge, the log text spilling out of its
    // box. While the pause is up the game is told it has the plain picture; on the way back the mode below restores it.
    if (N3DSPause_isPaused()) {
        Instance* control = N3DSGml_firstInstance(gAm2r.objControl);
        if (control != NULL) {
            double flag = 0.0, space = 0.0;
            if (N3DSGml_getVar(control, "widescreen", &flag) && flag != 0.0) N3DSGml_setVar(control, "widescreen", 0.0);
            if (N3DSGml_getVar(control, "widescreen_space", &space) && space != 0.0) N3DSGml_setVar(control, "widescreen_space", 0.0);
        }
    }
    N3DSAm2r_stopStraySpinLoops();
    N3DSAm2r_skipPauseFades();
    N3DSAm2r_applyScreenMode();
    N3DSAm2r_applyHud();
    N3DSAm2r_checkWidescreenSurface();
    N3DSAm2r_hideKeyboardRow();
    N3DSAm2r_updateCredits();
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
