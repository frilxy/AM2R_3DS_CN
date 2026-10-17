// AM2R 1.5.5's HUD (draw_gui) and light map (oLightEngine user event 1) in C. Included by n3ds_am2r_native.c, whose
// helpers (num, gmlCmp, drawSprite, ...) they use; both are pinned to their bytecode like the other natives.

// ===[ draw_gui ]===

// Diagnostic state: the screen mode the HUD last logged its layout for.
static int32_t gHudLoggedMode = -1;

static int32_t gCode_draw_gui = -1, gCode_gui_health = -1, gCode_to_string_lz = -1;

typedef struct {
    GMLReal classicmode, opshowhud, etanks, playerhealth, maxmissiles, missiles, opmslstyle, currentweapon, maxsmissiles,
        smissiles, maxpbombs, pbombs, ophudshowmap, ophudshowmetrcount, monstersarea, monstersleft;
} HudGlobals;

VAR(guifont1);
VAR(guifont1a);
GVAR(guifont2);
GVAR(missiles);
GVAR(smissiles);
GVAR(pbombs);
GVAR(classicmode);
GVAR(opshowhud);
GVAR(etanks);
GVAR(playerhealth);
GVAR(maxmissiles);
GVAR(opmslstyle);
GVAR(currentweapon);
GVAR(maxsmissiles);
GVAR(maxpbombs);
GVAR(ophudshowmap);
GVAR(ophudshowmetrcount);
VAR(mod_etankhealthmult);
VAR(hudflash);
VAR(hudflashfx);
VAR(widescreen_space);
VAR(state);
VAR(sjball);
VAR(armmsl);
VAR(xoff);
VAR(etankxoff);
GVAR(monstersarea);
GVAR(monstersleft);
VAR(widescreen);
VAR(surf);
VAR(scale);
VAR(flying_sprite);
VAR(yy);
VAR(xtype);

static bool globalNum(const Var* v, GMLReal* out) {
    return num(globalScope(), v, out);
}

static RValue callScript(int32_t code, RValue* args, int32_t argCount) {
    return VM_callCodeIndex(N.vm, code, args, argCount);
}

static void drawText(GMLReal x, GMLReal y, RValue text) {
    RValue a[3] = { R(x), R(y), text };
    a[2].ownsReference = false;
    callVoid(N.drawText, a, 3);
}

// draw_text(x, y, string(<value>))
static void drawTextOf(GMLReal x, GMLReal y, RValue value) {
    RValue arg = value;
    arg.ownsReference = false;
    RValue s = N.string(N.vm, &arg, 1);
    drawText(x, y, s);
    RValue_free(&s);
}

static void drawBackgroundAt(GMLReal bg, GMLReal x, GMLReal y) {
    RValue a[3] = { R(bg), R(x), R(y) };
    callVoid(N.drawBackground, a, 3);
}

static void setFont(RValue font) {
    RValue a[1] = { font };
    a[0].ownsReference = false;
    callVoid(N.drawSetFont, a, 1);
}

// The 30 energy tanks: x offset from etankxoff (the two rows share one), y by row.
static const int8_t kTankX[31] = { 0, 0, 0, 6, 6, 12, 12, 18, 18, 24, 24, 33, 33, 39, 39, 45, 45, 51, 51, 57, 57, 66, 66, 72,
    72, 78, 78, 84, 84, 90, 90 };

// The tank frame backgrounds by tank count (pairs), and the x the ammo row starts at after them.
static const struct { int16_t bg, xoff; } kTankFrames[16] = {
    { 117, 33 }, { 118, 42 }, { 119, 48 }, { 120, 54 }, { 121, 60 }, { 122, 66 }, { 291, 75 }, { 292, 81 }, { 293, 87 },
    { 294, 93 }, { 295, 99 }, { 296, 108 }, { 297, 114 }, { 298, 120 }, { 299, 126 }, { 300, 132 },
};

static RValue native_draw_gui(VMContext* ctx, RValue* args, int32_t argCount) {
    Instance* self = (Instance*) ctx->currentInstance;
    Instance* control = VM_findInstanceByTarget(ctx, N.objControl);
    Instance* character = VM_findInstanceByTarget(ctx, N.objCharacter);
    HudGlobals g;
    GMLReal mult = 0, hudflash = 0, hudflashfx = 0, ws = 0, state = 0, sjball = 0, armmsl = 0;
    RValue* guifont1 = control != NULL ? varSlot(control, &v_guifont1) : NULL;
    RValue* guifont1a = control != NULL ? varSlot(control, &v_guifont1a) : NULL;
    RValue* guifont2 = varSlot(globalScope(), &v_guifont2);
    RValue* missilesSlot = varSlot(globalScope(), &v_missiles);
    RValue* smissilesSlot = varSlot(globalScope(), &v_smissiles);
    RValue* pbombsSlot = varSlot(globalScope(), &v_pbombs);
    bool ok = self != NULL && control != NULL && character != NULL && globalNum(&v_classicmode, &g.classicmode) &&
        globalNum(&v_opshowhud, &g.opshowhud);
    if (ok && !(gmlCmp(g.classicmode, 0) == 0 && truthy(g.opshowhud))) return R(0.0); // HUD off: nothing drawn
    // Everything the game's code may read this frame, before drawing anything (a missing one: the game's code).
    ok = ok && globalNum(&v_etanks, &g.etanks) && globalNum(&v_playerhealth, &g.playerhealth) &&
        globalNum(&v_maxmissiles, &g.maxmissiles) && globalNum(&v_opmslstyle, &g.opmslstyle) &&
        globalNum(&v_currentweapon, &g.currentweapon) && globalNum(&v_maxsmissiles, &g.maxsmissiles) &&
        globalNum(&v_maxpbombs, &g.maxpbombs) && globalNum(&v_ophudshowmap, &g.ophudshowmap) &&
        globalNum(&v_ophudshowmetrcount, &g.ophudshowmetrcount) && num(control, &v_mod_etankhealthmult, &mult) &&
        num(control, &v_hudflash, &hudflash) && num(self, &v_widescreen_space, &ws) && num(character, &v_state, &state) &&
        num(character, &v_sjball, &sjball) && num(character, &v_armmsl, &armmsl) && guifont1 != NULL && guifont2 != NULL;
    if (ok && truthy(hudflash)) ok = guifont1a != NULL && num(control, &v_hudflashfx, &hudflashfx);
    if (ok && gmlCmp(g.maxmissiles, 0) > 0) ok = missilesSlot != NULL;
    if (ok && gmlCmp(g.maxsmissiles, 0) > 0) ok = smissilesSlot != NULL;
    if (ok && gmlCmp(g.maxpbombs, 0) > 0) ok = pbombsSlot != NULL;
    if (ok && gmlCmp(g.ophudshowmetrcount, 1) == 0) ok = globalNum(&v_monstersarea, &g.monstersarea);
    if (ok && gmlCmp(g.ophudshowmetrcount, 2) == 0) ok = globalNum(&v_monstersleft, &g.monstersleft);
    if (!ok) return callOriginal(gCode_draw_gui, args, argCount);

    // The tank frame (the game checks each pair; 29 also takes 30 and up). The whole left-hand block rides the picture's
    // transform in the game's own HUD, so however many columns the picture loses off the left are lost off this block
    // too: shift it back in by that much (N3DSAm2r_hudLeftInset; 0 unless the picture starts left of the screen). The
    // shift is a GMLReal (like every other HUD coordinate here) so it stays in the game's own arithmetic.
    GMLReal hudShift = (GMLReal) N3DSAm2r_hudLeftInset();
    GMLReal xoff = 33 + hudShift;
    setNum(self, &v_xoff, xoff);
    if (gmlCmp(g.etanks, 0) == 0) drawBackgroundAt(117, hudShift, 0);
    for (int32_t pair = 1; pair < 16; pair++) {
        int32_t lo = pair * 2 - 1;
        bool match = gmlCmp(g.etanks, lo) == 0 || (pair == 15 ? gmlCmp(g.etanks, 30) >= 0 : gmlCmp(g.etanks, lo + 1) == 0);
        if (!match) continue;
        drawBackgroundAt(kTankFrames[pair].bg, hudShift, 0);
        xoff = kTankFrames[pair].xoff + hudShift;
        setNum(self, &v_xoff, xoff);
    }

    // Health.
    setFont(*guifont1);
    RValue alpha[1] = { R(1) };
    callVoid(N.drawSetAlpha, alpha, 1);
    RValue colour[1] = { R(16777215) };
    callVoid(N.drawSetColor, colour, 1);
    RValue health = callScript(gCode_gui_health, NULL, 0);
    drawText(4 + hudShift, 4, health);
    RValue_free(&health);
    if (truthy(hudflash)) {
        setFont(*guifont1a);
        RValue flashText = callScript(gCode_gui_health, NULL, 0);
        RValue a[8] = { R(4 + hudShift), R(4), flashText, R(16777215), R(16777215), R(16777215), R(16777215), R(hudflashfx / 4) };
        a[2].ownsReference = false;
        callVoid(N.drawTextColor, a, 8);
        RValue_free(&flashText);
    }

    // Energy tanks: full when health is past the tank's share.
    setNum(self, &v_etankxoff, 32);
    GMLReal etankxoff = 32;
    for (int32_t k = 1; k <= 30; k++) {
        if (gmlCmp(g.etanks, k) < 0) continue;
        GMLReal threshold = k == 1 ? 99.9 : 99.9 + (GMLReal) (100 * (k - 1)) * mult;
        GMLReal x = (k <= 2 ? 0 + etankxoff : (0 + etankxoff) + kTankX[k]) + hudShift;
        GMLReal y = (k & 1) ? 4 : 10;
        if (gmlCmp(g.playerhealth, threshold) > 0) drawSprite(511, 1, x, y);
        if (gmlCmp(g.playerhealth, threshold) <= 0) drawSprite(511, 0, x, y);
    }

    // Ammo. "Busy" states (and spin-jump ball) grey the armed-weapon icon.
    setFont(*guifont2);
    bool busy = gmlCmp(state, 23) == 0 || gmlCmp(state, 24) == 0 || gmlCmp(state, 27) == 0 || gmlCmp(state, 54) == 0 ||
        gmlCmp(state, 55) == 0;
    bool styleHold = gmlCmp(g.opmslstyle, 0) == 0, styleToggle = gmlCmp(g.opmslstyle, 1) == 0;
    if (gmlCmp(g.maxmissiles, 0) > 0) {
        drawBackgroundAt(gmlCmp(g.maxmissiles, 100) >= 0 ? 126 : 127, xoff, 0);
        drawTextOf((0 + xoff) + 19, 7, *missilesSlot);
        GMLReal ix = (0 + xoff) + 1;
        if (styleHold) {
            if (gmlCmp(g.currentweapon, 1) != 0 || busy || truthy(sjball)) drawSprite(504, 0, ix, 4);
            if (gmlCmp(g.currentweapon, 1) == 0 && !busy && gmlCmp(sjball, 0) == 0) {
                if (gmlCmp(armmsl, 0) == 0) drawSprite(504, 1, ix, 4);
                if (gmlCmp(armmsl, 1) == 0) drawSprite(504, 2, ix, 4);
            }
        }
        if (styleToggle) drawSprite(504, gmlCmp(g.currentweapon, 1) == 0 ? 1 : 0, ix, 4);
        xoff = xoff + (gmlCmp(g.maxmissiles, 100) >= 0 ? 45 : 38);
        setNum(self, &v_xoff, xoff);
    }
    if (gmlCmp(g.maxsmissiles, 0) > 0) {
        drawBackgroundAt(127, xoff, 0);
        drawTextOf(xoff + 19, 7, *smissilesSlot);
        GMLReal ix = xoff + 1;
        if (styleHold) {
            if (gmlCmp(g.currentweapon, 2) != 0 || busy || truthy(sjball)) drawSprite(505, 0, ix, 4);
            if (gmlCmp(g.currentweapon, 2) == 0 && !busy && gmlCmp(sjball, 0) == 0) {
                if (gmlCmp(armmsl, 0) == 0) drawSprite(505, 1, ix, 4);
                if (gmlCmp(armmsl, 1) == 0) drawSprite(505, 2, ix, 4);
            }
        }
        if (styleToggle) drawSprite(505, gmlCmp(g.currentweapon, 2) == 0 ? 1 : 0, ix, 4);
        xoff = xoff + 38;
        setNum(self, &v_xoff, xoff);
    }
    if (gmlCmp(g.maxpbombs, 0) > 0) {
        drawBackgroundAt(127, xoff, 0);
        drawTextOf(xoff + 19, 7, *pbombsSlot);
        GMLReal ix = xoff + 1;
        if (styleHold) {
            if (!busy && gmlCmp(sjball, 0) == 0) drawSprite(506, 0, ix, 4);
            if (busy || truthy(sjball)) {
                if (gmlCmp(armmsl, 0) == 0) drawSprite(506, 1, ix, 4);
                if (gmlCmp(armmsl, 1) == 0) drawSprite(506, 2, ix, 4);
            }
        }
        if (styleToggle) drawSprite(506, gmlCmp(g.currentweapon, 3) == 0 ? 1 : 0, ix, 4);
    }

    // Map and Metroid counter frames. The game lays these out at 250 + widescreen_space, for a picture as wide as its
    // own. Two things move them back in:
    //
    //  - point-for-point and stretch draw the game's plain 320x240 picture, so the space is cancelled outright and the
    //    block ends at 320, flush with the picture's right edge.
    //  - wide keeps the game's 426-wide picture and only gives up the columns the screen crops, which is what
    //    N3DSAm2r_hudRightInset reports.
    //
    // The first case is decided here rather than from a value cached in applyHud: the game rewrites widescreen_space in
    // its own Step, so a figure read there can disagree with the one the layout actually used - which is what pushed
    // the block 106 columns right, over the black margin and off the screen's edge.
    //
    // ws is GMLReal (float) and -Werror=double-promotion is on, so the inset is narrowed rather than promoting ws.
    N3DSScreenMode hudScreenMode = N3DS_getScreenMode();
    GMLReal rawSpace = ws;
    if (hudScreenMode == N3DS_SCREEN_1X || hudScreenMode == N3DS_SCREEN_STRETCH) {
        ws = 0;
    } else {
        ws -= (GMLReal) N3DSAm2r_hudRightInset();
    }
    // Diagnostic, once per mode change: the space the game reports, the space this ends up using, and the x each
    // frame is drawn at. Read against the "gui canvas ... -> viewport" line above it, this separates "drawn in the
    // wrong place" from "blitted in the wrong place".
    if ((int32_t) hudScreenMode != gHudLoggedMode) {
        gHudLoggedMode = (int32_t) hudScreenMode;
        logInfo("HUD: mode %d space %.2f used %.2f -> frames at %.1f / %.1f / %.1f\n",
            (int) hudScreenMode, (double) rawSpace, (double) ws, (double) (250 + ws), (double) (276 + ws),
            (double) (296 + ws));
    }
    bool showMap = truthy(g.ophudshowmap), showCount = truthy(g.ophudshowmetrcount);
    if (showMap && showCount) {
        drawBackgroundAt(123, 250 + ws, 0);
        xoff = 250;
        setNum(self, &v_xoff, xoff);
    }
    if (showMap && gmlCmp(g.ophudshowmetrcount, 0) == 0) drawBackgroundAt(124, 272 + ws, 0);
    if (gmlCmp(g.ophudshowmap, 0) == 0 && showCount) {
        drawBackgroundAt(125, 296 + ws, 0);
        xoff = 296;
        setNum(self, &v_xoff, xoff);
    }
    if (showCount) {
        for (int32_t which = 1; which <= 2; which++) {
            if (gmlCmp(g.ophudshowmetrcount, which) != 0) continue;
            drawBackgroundAt(which == 1 ? 128 : 129, (xoff + 4) + ws, 4);
            RValue n = R(which == 1 ? g.monstersarea : g.monstersleft);
            RValue text = callScript(gCode_to_string_lz, &n, 1);
            drawText((xoff + 6) + ws, 21, text);
            RValue_free(&text);
        }
    }
    if (showMap) {
        RValue mapArgs[2] = { R(276 + ws), R(0) };
        RValue r = native_draw_gui_map(ctx, mapArgs, 2);
        RValue_free(&r);
    }
    return R(0.0);
}

// ===[ oLightEngine user event 1: the light map ]===
// Clears the light surface to white, subtracts a dark frame, then for each light-giving object draws a soft light
// sprite four times, mirrored (+x+y, -x+y, +x-y, -x-y), at its position in the view. A room with the final boss's
// core (oCoreX: nested with-blocks) runs the game's code.

static int32_t gObjLight, gObjBeam, gObjMissile, gObjBomb, gObjBomb2, gObjPickup, gObjMGammaElec, gObjGlowPlant1,
    gObjSpikePlant, gObjPincherFly, gObjA3LabLight, gObjA3LabDoor, gObjFXAnimSpark, gObjLightBug, gObjChargeBeamSpark1,
    gObjItemBall, gObjItem, gObjDoor, gObjMOmegaFlame, gObjMOmegaProjectile, gObjA8Lamp, gObjA8RedLight, gObjA6Dust,
    gObjA8RedLightFX, gObjGenesisAcid, gObjGenesisSlashProj, gObjElderSeptogg, gObjXPickup, gObjCoreX;

static Instance** gWithList = NULL; // stb_ds scratch

// The instances a with(obj) visits, in the VM's order (one or two: in order; more: last first).
static int32_t withInstances(int32_t obj) {
    arrsetlen(gWithList, 0);
    if (obj < 0 || (uint32_t) obj >= N.runner->dataWin->objt.count) return 0;
    Instance** source = N.runner->instancesByObject[obj];
    int32_t n = (int32_t) arrlen(source);
    for (int32_t i = 0; i < n; i++) {
        if (source[i]->active) arrput(gWithList, source[i]);
    }
    int32_t count = (int32_t) arrlen(gWithList);
    if (count > 2) {
        for (int32_t i = 0, j = count - 1; i < j; i++, j--) {
            Instance* t = gWithList[i];
            gWithList[i] = gWithList[j];
            gWithList[j] = t;
        }
    }
    return count;
}

static GMLReal gViewX, gViewY;

static void lightQuad(GMLReal sprite, GMLReal x, GMLReal y, GMLReal sx, GMLReal sy, GMLReal rot, GMLReal colour, GMLReal alpha) {
    drawSpriteExt(sprite, -1, x, y, sx, sy, rot, colour, alpha);
    drawSpriteExt(sprite, -1, x, y, -sx, sy, rot, colour, alpha);
    drawSpriteExt(sprite, -1, x, y, sx, -sy, rot, colour, alpha);
    drawSpriteExt(sprite, -1, x, y, -sx, -sy, rot, colour, alpha);
}

static GMLReal colourRgb(GMLReal r, GMLReal g, GMLReal b) {
    RValue a[3] = { R(r), R(g), R(b) };
    RValue c = N.makeColorRgb(N.vm, a, 3);
    GMLReal v = RValue_toReal(c);
    RValue_free(&c);
    return v;
}

static GMLReal colourHsv(GMLReal h, GMLReal s, GMLReal v) {
    RValue a[3] = { R(h), R(s), R(v) };
    RValue c = N.makeColorHsv(N.vm, a, 3);
    GMLReal out = RValue_toReal(c);
    RValue_free(&c);
    return out;
}

// x - view_xview[w], y - view_yview[w] (+ an offset to y or x, added first, as the game's code does).
#define LX(inst, dx) (((GMLReal) (inst)->x + (dx)) - gViewX)
#define LY(inst, dy) (((GMLReal) (inst)->y + (dy)) - gViewY)

// Simple blocks: every instance of obj gets a quad of sprite 1127 at scale s (y offset dy), colour from a fixed value
// or its image_blend (colour < 0), alpha 1.
static void lightSimple(int32_t obj, GMLReal s, GMLReal dx, GMLReal dy, GMLReal colour) {
    int32_t n = withInstances(obj);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        lightQuad(1127, dx != 0 ? LX(inst, dx) : (GMLReal) inst->x - gViewX, dy != 0 ? LY(inst, dy) : (GMLReal) inst->y - gViewY,
            s, s, 0, colour < 0 ? (GMLReal) inst->imageBlend : colour, 1);
    }
}

// Checks what the light blocks read from instances before drawing anything.
static const char* lightInputsProblem(void) {
    int32_t n = withInstances(gObjCoreX);
    if (n > 0) return "oCoreX";
    n = withInstances(gObjA6Dust);
    for (int32_t i = 0; i < n; i++) {
        GMLReal v;
        if (!num(gWithList[i], &v_scale, &v)) return "oA6Dust.scale";
    }
    n = withInstances(gObjElderSeptogg);
    for (int32_t i = 0; i < n; i++) {
        GMLReal f, yy;
        if (!num(gWithList[i], &v_flying_sprite, &f)) return "oElderSeptogg.flying_sprite";
        if (gmlCmp(f, 1576) == 0 && !num(gWithList[i], &v_yy, &yy)) return "oElderSeptogg.yy";
    }
    n = withInstances(gObjXPickup);
    for (int32_t i = 0; i < n; i++) {
        GMLReal t;
        if (!num(gWithList[i], &v_xtype, &t)) return "oXPickup.xtype";
    }
    return NULL;
}

// Once per kind of reason, so a room that always hands over says why.
static void lightDeclined(const char* why) {
    static const char* lastWhy = NULL;
    if (why == lastWhy) return;
    lastWhy = why;
    logInfo("AM2R natives: light map left to the game's code (%s)\n", why);
}

static bool native_oLightEngine_Other_11(MAYBE_UNUSED Runner* runner, Instance* self) {
    Instance* control = VM_findInstanceByTarget(N.vm, N.objControl);
    RValue* widescreenSlot = control != NULL ? varSlot(control, &v_widescreen) : NULL;
    RValue* surfSlot = varSlot(self, &v_surf);
    if (widescreenSlot == NULL || surfSlot == NULL) {
        lightDeclined(widescreenSlot == NULL ? "oControl.widescreen" : "surf");
        return false;
    }
    const char* problem = lightInputsProblem();
    if (problem != NULL) {
        lightDeclined(problem);
        return false;
    }
    int32_t w = RValue_toInt32(*widescreenSlot);
    RValue vx = VMBuiltins_getVariable(N.vm, self, b_view_xview, "view_xview", w);
    RValue vy = VMBuiltins_getVariable(N.vm, self, b_view_yview, "view_yview", w);
    gViewX = RValue_toReal(vx);
    gViewY = RValue_toReal(vy);
    RValue_free(&vx);
    RValue_free(&vy);

    RValue target[1] = { *surfSlot };
    target[0].ownsReference = false;
    callVoid(N.surfaceSetTarget, target, 1);
    RValue white[1] = { R(16777215) };
    callVoid(N.drawClear, white, 1);
    blendMode(3);
    RValue one[1] = { R(1) };
    callVoid(N.drawSetAlpha, one, 1);
    RValue dark[1] = { R(colourHsv(0, 0, 1)) };
    callVoid(N.drawSetColor, dark, 1);
    RValue rect[5] = { R(0), R(0), R(660), R(250), R(0) };
    callVoid(N.drawRectangle, rect, 5);

    int32_t n;
    // oLight: image_blend = make_color_hsv(0, 0, image_alpha * 255), its own sprite and scale.
    n = withInstances(gObjLight);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        VMBuiltins_setVariable(N.vm, inst, b_image_blend, "image_blend", R(colourHsv(0, 0, (GMLReal) inst->imageAlpha * 255)), -1);
        lightQuad(inst->spriteIndex, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, inst->imageXscale,
            inst->imageYscale, 0, (GMLReal) inst->imageBlend, 1);
    }
    lightSimple(gObjBeam, 0.5, 0, 0, -1);
    lightSimple(gObjMissile, 0.5, 0, 0, -1);
    lightSimple(gObjBomb, 0.25, 0, 0, -1);
    lightSimple(gObjBomb2, 0.25, 0, 0, -1);
    lightSimple(gObjPickup, 0.25, 0, 0, -1);
    // oMGammaElec: one copy of sprite 1129, rotated with the instance.
    n = withInstances(gObjMGammaElec);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        drawSpriteExt(1129, -1, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, 1, 1, inst->imageAngle,
            (GMLReal) inst->imageBlend, 1);
    }
    lightSimple(gObjGlowPlant1, 0.65, 0, -16, colourRgb(100, 100, 250));
    lightSimple(gObjSpikePlant, 0.4, 0, 0, colourRgb(150, 150, 60));
    lightSimple(gObjPincherFly, 0.3, 0, 0, colourRgb(120, 80, 30));
    // oA3LabLight: only while on (image_index 0).
    n = withInstances(gObjA3LabLight);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        if (gmlCmp(inst->imageIndex, 0) != 0) continue;
        lightQuad(1127, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, 1.4, 1.4, 0, colourRgb(230, 230, 130), 1);
    }
    // Doors: a flat light (1 x 0.4) at y + 32.
    for (int32_t pass = 0; pass < 1; pass++) {
        n = withInstances(gObjA3LabDoor);
        for (int32_t i = 0; i < n; i++) {
            Instance* inst = gWithList[i];
            if (i > 0 && !inst->active) continue;
            lightQuad(1127, (GMLReal) inst->x - gViewX, LY(inst, 32), 1, 0.4, 0, 16777215, 1);
        }
    }
    // oFXAnimSpark (sprites 92 and 94): fading with its animation.
    n = withInstances(gObjFXAnimSpark);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        if (gmlCmp(inst->spriteIndex, 92) != 0 && gmlCmp(inst->spriteIndex, 94) != 0) continue;
        GMLReal lum = (1 - ((GMLReal) inst->imageIndex / 11) * 220) + 35;
        lightQuad(1127, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, 0.5, 0.5, 0, colourRgb(lum, lum, lum), 1);
    }
    lightSimple(gObjLightBug, 0.25, 0, 0, 16777215);
    lightSimple(gObjChargeBeamSpark1, 0.5, 0, 0, -1);
    lightSimple(gObjItemBall, 0.5, 8, -8, -1);
    lightSimple(gObjItem, 0.5, 8, -8, -1);
    n = withInstances(gObjDoor);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        lightQuad(1127, (GMLReal) inst->x - gViewX, LY(inst, 32), 1, 0.4, 0, 16777215, 1);
    }
    lightSimple(gObjMOmegaFlame, 0.75, 0, 0, -1);
    // Omega's projectiles: full size, their own alpha, at y + 16.
    n = withInstances(gObjMOmegaProjectile);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        lightQuad(1127, (GMLReal) inst->x - gViewX, LY(inst, 16), 1, 1, 0, (GMLReal) inst->imageBlend, inst->imageAlpha);
    }
    // Area 8 lamps: the light at y + 4, then a wide beam (sprite 297) both ways.
    n = withInstances(gObjA8Lamp);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        GMLReal blend = (GMLReal) inst->imageBlend, x = (GMLReal) inst->x - gViewX;
        lightQuad(1127, x, LY(inst, 4), 0.5, 0.5, 0, blend, 1);
        drawSpriteExt(297, -1, x, (GMLReal) inst->y - gViewY, 2, 1, 0, blend, 1);
        drawSpriteExt(297, -1, x, (GMLReal) inst->y - gViewY, -2, 1, 0, blend, 1);
    }
    // Area 8 red lights: red by their alpha.
    n = withInstances(gObjA8RedLight);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        GMLReal colour = colourRgb((GMLReal) inst->imageAlpha * 255, 0, 0);
        lightQuad(1127, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, 1, 1, 0, colour, inst->imageAlpha);
    }
    // Area 6 dust: greenish, by alpha and its own scale, at whole pixels.
    n = withInstances(gObjA6Dust);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        GMLReal a = inst->imageAlpha, scale = 0;
        num(inst, &v_scale, &scale);
        GMLReal colour = colourRgb(a * 133, a * 189, a * 136);
        GMLReal x = GMLReal_bankersRound((GMLReal) inst->x - gViewX), y = GMLReal_bankersRound((GMLReal) inst->y - gViewY);
        drawSpriteExt(1127, -1, x, y, scale * 1.2, scale * 1.2, 0, colour, a);
        drawSpriteExt(1127, -1, x, y, -scale * 1.2, scale * 1.2, 0, colour, a);
        drawSpriteExt(1127, -1, x, y, scale * 1.2, -scale * 1.2, 0, colour, a);
        drawSpriteExt(1127, -1, x, y, -scale * 1.2, -scale * 1.2, 0, colour, a);
    }
    // Area 8 red light flash: a full-screen background.
    n = withInstances(gObjA8RedLightFX);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        RValue a[8] = { R(151), R(0), R(0), R(20), R(20), R(0), R((GMLReal) inst->imageBlend), R(inst->imageAlpha) };
        callVoid(N.drawBackgroundExt, a, 8);
    }
    lightSimple(gObjGenesisAcid, 0.25, 0, 0, 65280);
    n = withInstances(gObjGenesisSlashProj);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        GMLReal colour = colourRgb(0, (GMLReal) inst->imageAlpha * 255, 0);
        lightQuad(1127, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, 0.5, 0.5, 0, colour, 1);
    }
    // Elder Septoggs: while flying, a light under them (from xstart, y + yy + 21).
    n = withInstances(gObjElderSeptogg);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        GMLReal f = 0, yy = 0;
        num(inst, &v_flying_sprite, &f);
        if (gmlCmp(f, 1576) != 0) continue;
        num(inst, &v_yy, &yy);
        lightQuad(1127, (GMLReal) inst->xstart - gViewX, (((GMLReal) inst->y + yy) + 21) - gViewY, 0.5, 0.5, 0, 4235519, 0.8);
    }
    // Metroid DNA / X pickups: coloured by kind.
    n = withInstances(gObjXPickup);
    for (int32_t i = 0; i < n; i++) {
        Instance* inst = gWithList[i];
        if (i > 0 && !inst->active) continue;
        GMLReal t = 0;
        num(inst, &v_xtype, &t);
        GMLReal colour = gmlCmp(t, 1477) == 0 ? 65535 : gmlCmp(t, 1478) == 0 ? 65280 : gmlCmp(t, 1479) == 0 ? 4235519
            : gmlCmp(t, 1480) == 0 ? 255 : -1;
        if (colour < 0) continue;
        lightQuad(1127, (GMLReal) inst->x - gViewX, (GMLReal) inst->y - gViewY, 0.3, 0.3, 0, colour, 1);
    }

    blendMode(0);
    callVoid(N.surfaceResetTarget, NULL, 0);
    return true;
}
