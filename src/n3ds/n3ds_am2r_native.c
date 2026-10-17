#include "n3ds_am2r_native.h"

#include "n3ds_am2r.h"
#include "n3ds_gml.h"
#include "n3ds_screen.h"

#include "../instance.h"
#include "../int_rvalue_hashmap.h"
#include "../log.h"
#include "../runner.h"
#include "../utils.h"
#include "../vm.h"
#include "../vm_builtins.h"

#include <stb/ds/stb_ds.h>
#include <math.h>
#include <string.h>

// Each native below follows the game's bytecode instruction for instruction (the disassembly it was written from is
// quoted in its comment where the order matters): the same values read, the same variables written (with the same
// side effects: builtins go through the VM's own setter), the same builtins called in the same order (random() keeps
// the same sequence). Numbers compare like GML (within GML_MATH_EPSILON).

// ===[ State ]===

typedef struct {
    const char* name;      // the script's name (FUNC entry), or the event's code name
    BuiltinFunc script;    // a script native (called like a builtin), or
    NativeEventCode event; // an event native
    uint32_t hash;         // FNV-1a of the bytecode it was written from
    int32_t code;          // code index, -1 if the game has no such code
    bool matches;
} NativeEntry;

static struct {
    Runner* runner;
    VMContext* vm;
    bool enabled;
    bool initialised;
    int32_t available;
    NativeEventCode* events; // by code index, handed to the runner while enabled
    // FUNC entries of the hooked scripts and what the VM had for them before.
    struct { int32_t funcIndex; int32_t entry; void* original; }* hooks; // stb_ds array
    // Builtins the natives call.
    BuiltinFunc collisionLine, drawSprite, drawSpriteExt, drawBackground, drawSetBlendMode, drawSetColourWriteEnable,
        random, drawText, drawTextColor, drawSetFont, drawSetAlpha, drawSetColor, string, makeColorRgb, makeColorHsv,
        surfaceSetTarget, surfaceResetTarget, drawClear, drawRectangle, drawBackgroundExt;
    // Objects.
    int32_t objSolid, objControl, objCharacter;
} N;

// ===[ Variables ]===

typedef struct {
    const char* name;
    int32_t id;
    bool global; // global.<name> rather than an instance variable
} Var;

#define VAR(n) static Var v_##n = { #n, -1, false }
#define GVAR(n) static Var v_##n = { #n, -1, true }
VAR(lb); VAR(tb); VAR(rb); VAR(bb);
VAR(collisionBoundsOffsetLeftX); VAR(collisionBoundsOffsetTopY); VAR(collisionBoundsOffsetRightX); VAR(collisionBoundsOffsetBottomY);
VAR(i); VAR(w1); VAR(h1); VAR(w2); VAR(h2);
VAR(flipx); VAR(facing); VAR(frozen); VAR(frozenspr); VAR(myspr); VAR(flashing); VAR(freezetime); VAR(fxtimer);
VAR(malpha); VAR(moffx); VAR(moffy); VAR(markfr); GVAR(mapmarker);
static Var v_m[16], v_dm[16];
static char gMapVarNames[32][8];

static int16_t b_image_xscale = -1, b_image_blend = -1, b_view_xview = -1, b_view_yview = -1;

// The variable's id from the VARI entry of its kind: the VM's name map also holds locals (1.5.5 has a local "scale"),
// and the first entry of a name wins there.
static void resolveVar(Var* v) {
    v->id = -1;
    if (N.runner == NULL || v->name == NULL) return;
    Vari* vari = &N.runner->dataWin->vari;
    int32_t fallback = -1;
    for (uint32_t k = 0; k < vari->variableCount; k++) {
        Variable* e = &vari->variables[k];
        if (e->varID < 0 || e->instanceType == INSTANCE_LOCAL || e->name == NULL || strcmp(e->name, v->name) != 0) continue;
        bool isGlobal = e->instanceType == INSTANCE_GLOBAL;
        if (isGlobal == v->global) {
            v->id = e->varID;
            return;
        }
        if (fallback < 0) fallback = e->varID;
    }
    v->id = fallback;
}

static inline RValue* varSlot(Instance* inst, const Var* v) {
    if (inst == NULL || v->id < 0) return NULL;
    return IntRValueHashMap_findSlot(&inst->selfVars, v->id);
}

static inline bool isNumber(const RValue* r) {
    return r->type == RVALUE_REAL || r->type == RVALUE_INT32 || r->type == RVALUE_INT64 || r->type == RVALUE_BOOL;
}

// inst.<v> as a number; false if it isn't set or isn't a number (the native then hands over to the game's code).
static inline bool num(Instance* inst, const Var* v, GMLReal* out) {
    RValue* s = varSlot(inst, v);
    if (s == NULL || !isNumber(s)) return false;
    *out = RValue_toReal(*s);
    return true;
}

static inline const char* str(Instance* inst, const Var* v) {
    RValue* s = varSlot(inst, v);
    return s != NULL && s->type == RVALUE_STRING && s->string != NULL ? s->string : NULL;
}

// A numeric store, as Pop.v.v / Pop.v.i leave it (the VM stores int constants as reals).
static inline void setNum(Instance* inst, const Var* v, GMLReal value) {
    if (v->id >= 0) Instance_setSelfVar(inst, v->id, RValue_makeReal(value));
}

static inline Instance* globalScope(void) {
    return (Instance*) N.vm->globalScopeInstance;
}

static inline int gmlCmp(GMLReal a, GMLReal b) {
    GMLReal d = a - b;
    return GMLReal_fabs(d) <= GML_MATH_EPSILON ? 0 : (d < 0 ? -1 : 1);
}

static inline bool truthy(GMLReal v) {
    return v > 0.5;
}

static inline bool argNum(RValue* args, int32_t argCount, int32_t i, GMLReal* out) {
    if (i >= argCount || !isNumber(&args[i])) return false;
    *out = RValue_toReal(args[i]);
    return true;
}

// ===[ Builtin calls ]===

static BuiltinFunc builtin(const char* name) {
    ptrdiff_t slot = shgeti(N.vm->builtinMap, (char*) name);
    return slot >= 0 ? N.vm->builtinMap[slot].value : NULL;
}

static inline void callVoid(BuiltinFunc f, RValue* args, int32_t argCount) {
    RValue r = f(N.vm, args, argCount);
    RValue_free(&r);
}

static inline RValue R(GMLReal v) {
    return RValue_makeReal(v);
}

static void drawSprite(GMLReal sprite, GMLReal sub, GMLReal x, GMLReal y) {
    RValue a[4] = { R(sprite), R(sub), R(x), R(y) };
    callVoid(N.drawSprite, a, 4);
}

static void drawSpriteExt(GMLReal sprite, GMLReal sub, GMLReal x, GMLReal y, GMLReal xs, GMLReal ys, GMLReal rot, GMLReal colour, GMLReal alpha) {
    RValue a[9] = { R(sprite), R(sub), R(x), R(y), R(xs), R(ys), R(rot), R(colour), R(alpha) };
    callVoid(N.drawSpriteExt, a, 9);
}

static void drawSpriteExtValue(RValue sprite, GMLReal sub, GMLReal x, GMLReal y, GMLReal xs, GMLReal ys, GMLReal rot, GMLReal colour, GMLReal alpha) {
    RValue a[9] = { sprite, R(sub), R(x), R(y), R(xs), R(ys), R(rot), R(colour), R(alpha) };
    a[0].ownsReference = false;
    callVoid(N.drawSpriteExt, a, 9);
}

static void blendMode(GMLReal mode) {
    RValue a[1] = { R(mode) };
    callVoid(N.drawSetBlendMode, a, 1);
}

// The game's own code for a hooked script (the native's way out).
static RValue callOriginal(int32_t code, RValue* args, int32_t argCount) {
    return VM_callCodeIndex(N.vm, code, args, argCount);
}

// ===[ Scripts ]===

static int32_t gCode_approximatelyZero = -1, gCode_calculateCollisionBounds = -1, gCode_isCollisionLeft = -1,
    gCode_isCollisionRight = -1, gCode_isCollisionTop = -1, gCode_isCollisionBottom = -1,
    gCode_isCollisionRectangle = -1, gCode_string_split = -1, gCode_draw_mapblock = -1, gCode_draw_gui_map = -1;

// approximatelyZero(v): return argument0 > -0.1 && argument0 < 0.1.
static RValue native_approximatelyZero(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    GMLReal v;
    if (!argNum(args, argCount, 0, &v)) return callOriginal(gCode_approximatelyZero, args, argCount);
    return R(gmlCmp(v, -0.1) > 0 && gmlCmp(v, 0.1) < 0 ? 1.0 : 0.0);
}

// calculateCollisionBounds(): lb/tb/rb/bb = x/y + collisionBoundsOffset*.
static bool calcBounds(Instance* self) {
    GMLReal l, t, r, b;
    if (!num(self, &v_collisionBoundsOffsetLeftX, &l) || !num(self, &v_collisionBoundsOffsetTopY, &t) ||
        !num(self, &v_collisionBoundsOffsetRightX, &r) || !num(self, &v_collisionBoundsOffsetBottomY, &b)) return false;
    GMLReal x = self->x, y = self->y;
    setNum(self, &v_lb, x + l);
    setNum(self, &v_tb, y + t);
    setNum(self, &v_rb, x + r);
    setNum(self, &v_bb, y + b);
    return true;
}

static RValue native_calculateCollisionBounds(VMContext* ctx, RValue* args, int32_t argCount) {
    Instance* self = (Instance*) ctx->currentInstance;
    if (self == NULL || !calcBounds(self)) return callOriginal(gCode_calculateCollisionBounds, args, argCount);
    return R(0.0);
}

// collision_line(round(x1), round(y1), round(x2), round(y2), oSolid, 1, 1) > 0
static bool solidLine(GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2) {
    RValue a[7] = { R(GMLReal_bankersRound(x1)), R(GMLReal_bankersRound(y1)), R(GMLReal_bankersRound(x2)),
        R(GMLReal_bankersRound(y2)), R((GMLReal) N.objSolid), R(1.0), R(1.0) };
    RValue r = N.collisionLine(N.vm, a, 7);
    bool hit = gmlCmp(RValue_toReal(r), 0.0) > 0;
    RValue_free(&r);
    return hit;
}

typedef enum { SIDE_LEFT, SIDE_RIGHT, SIDE_TOP, SIDE_BOTTOM } Side;

static RValue isCollisionSide(VMContext* ctx, RValue* args, int32_t argCount, Side side, int32_t code) {
    Instance* self = (Instance*) ctx->currentInstance;
    GMLReal a0, lb, tb, rb, bb;
    if (self == NULL || !argNum(args, argCount, 0, &a0) || !calcBounds(self) || !num(self, &v_lb, &lb) ||
        !num(self, &v_tb, &tb) || !num(self, &v_rb, &rb) || !num(self, &v_bb, &bb)) {
        return callOriginal(code, args, argCount);
    }
    bool hit = false;
    switch (side) {
        // collision_line(round(lb - a0), round(tb), round(lb - a0), round(bb - 1), ...)
        case SIDE_LEFT: hit = solidLine(lb - a0, tb, lb - a0, bb - 1); break;
        // collision_line(round(rb + a0 - 1), round(tb), round(rb + a0 - 1), round(bb - 1), ...)
        case SIDE_RIGHT: hit = solidLine(rb + a0 - 1, tb, rb + a0 - 1, bb - 1); break;
        // collision_line(round(lb), round(tb - a0), round(rb - 1), round(tb - a0), ...)
        case SIDE_TOP: hit = solidLine(lb, tb - a0, rb - 1, tb - a0); break;
        // collision_line(round(lb), round(bb + a0 - 1), round(rb - 1), round(bb + a0 - 1), ...)
        case SIDE_BOTTOM: hit = solidLine(lb, bb + a0 - 1, rb - 1, bb + a0 - 1); break;
    }
    return R(hit ? 1.0 : 0.0);
}

static RValue native_isCollisionLeft(VMContext* ctx, RValue* args, int32_t argCount) {
    return isCollisionSide(ctx, args, argCount, SIDE_LEFT, gCode_isCollisionLeft);
}
static RValue native_isCollisionRight(VMContext* ctx, RValue* args, int32_t argCount) {
    return isCollisionSide(ctx, args, argCount, SIDE_RIGHT, gCode_isCollisionRight);
}
static RValue native_isCollisionTop(VMContext* ctx, RValue* args, int32_t argCount) {
    return isCollisionSide(ctx, args, argCount, SIDE_TOP, gCode_isCollisionTop);
}
static RValue native_isCollisionBottom(VMContext* ctx, RValue* args, int32_t argCount) {
    return isCollisionSide(ctx, args, argCount, SIDE_BOTTOM, gCode_isCollisionBottom);
}

// isCollisionRectangle(x1, y1, x2, y2, x3, y3, x4, y4): w1/h1/w2/h2 are self variables in the game's code.
static RValue native_isCollisionRectangle(VMContext* ctx, RValue* args, int32_t argCount) {
    Instance* self = (Instance*) ctx->currentInstance;
    GMLReal a[8];
    bool ok = self != NULL;
    for (int32_t k = 0; ok && k < 8; k++) ok = argNum(args, argCount, k, &a[k]);
    if (!ok) return callOriginal(gCode_isCollisionRectangle, args, argCount);
    GMLReal w1 = a[2] - a[0], h1 = a[3] - a[1], w2 = a[6] - a[4], h2 = a[7] - a[5];
    setNum(self, &v_w1, w1);
    setNum(self, &v_h1, h1);
    setNum(self, &v_w2, w2);
    setNum(self, &v_h2, h2);
    if (gmlCmp(w2, 0) <= 0 || gmlCmp(h2, 0) <= 0 || gmlCmp(w1, 0) <= 0 || gmlCmp(h1, 0) <= 0) return R(0.0);
    w2 += a[4];
    h2 += a[5];
    w1 += a[0];
    h1 += a[1];
    setNum(self, &v_w2, w2);
    setNum(self, &v_h2, h2);
    setNum(self, &v_w1, w1);
    setNum(self, &v_h1, h1);
    bool hit = (gmlCmp(w2, a[4]) < 0 || gmlCmp(w2, a[0]) > 0) && (gmlCmp(h2, a[5]) < 0 || gmlCmp(h2, a[1]) > 0) &&
        (gmlCmp(w1, a[0]) < 0 || gmlCmp(w1, a[4]) > 0) && (gmlCmp(h1, a[1]) < 0 || gmlCmp(h1, a[5]) > 0);
    return R(hit ? 1.0 : 0.0);
}

static bool isAscii(const char* s) {
    for (; *s != '\0'; s++) {
        if ((unsigned char) *s >= 0x80) return false;
    }
    return true;
}

// string_split(str, n, delimiter): the n-th field of str. The game's loop, with its quirks (the character after a
// delimiter is taken without being checked, self.i is left past the end):
//   i = 1; while (i < string_length(str) + 1) { if (string_char_at(str, i) == delim) { sep++; i++ }
//                                               if (sep == n) out += string_char_at(str, i); i++ }
static RValue native_string_split(VMContext* ctx, RValue* args, int32_t argCount) {
    Instance* self = (Instance*) ctx->currentInstance;
    GMLReal n;
    if (self == NULL || argCount < 3 || args[0].type != RVALUE_STRING || args[0].string == NULL ||
        args[2].type != RVALUE_STRING || args[2].string == NULL || !argNum(args, argCount, 1, &n) ||
        !isAscii(args[0].string) || !isAscii(args[2].string)) {
        return callOriginal(gCode_string_split, args, argCount);
    }
    const char* s = args[0].string;
    const char* delim = args[2].string;
    int32_t len = (int32_t) strlen(s);
    char* out = (char*) safeMalloc((size_t) len + 1);
    int32_t outLen = 0;
    int32_t sep = 0;
    int32_t i = 1;
    while (len + 1 > i) {
        // string_char_at(s, i) == delim: one character against the whole delimiter string.
        if (delim[0] == s[i - 1] && delim[1] == '\0' && s[i - 1] != '\0') {
            sep++;
            i++;
        }
        if (gmlCmp((GMLReal) sep, n) == 0 && len >= i) out[outLen++] = s[i - 1];
        i++;
    }
    out[outLen] = '\0';
    setNum(self, &v_i, (GMLReal) i);
    return RValue_makeOwnedString(out);
}

// draw_mapblock(x, y, c1, c2, c3, c4, c5, c6, explored, c7): one cell of the HUD map, from its 7 map characters.
static void mapBlock(GMLReal x, GMLReal y, const char* c[10], GMLReal dm) {
    if (gmlCmp(dm, 0) <= 0) return;
    // argument6 (c5): the cell's base, sprite 492.
    if (strcmp(c[6], "1") == 0) drawSprite(492, 0, x, y);
    else if (strcmp(c[6], "2") == 0) drawSprite(492, 1, x, y);
    else if (strcmp(c[6], "3") == 0) drawSprite(492, 2, x, y);
    else if (strcmp(c[6], "4") == 0) drawSprite(492, 3, x, y);
    // argument9 (c7): the room shape, sprite 498 frames "1".."9", "A".."Z", "a".."h".
    const char* s9 = c[9];
    if (s9[0] != '\0' && s9[1] == '\0') {
        char ch = s9[0];
        int32_t frame = -1;
        if (ch >= '1' && ch <= '9') frame = ch - '1';
        else if (ch >= 'A' && ch <= 'Z') frame = 9 + (ch - 'A');
        else if (ch >= 'a' && ch <= 'h') frame = 35 + (ch - 'a');
        if (frame >= 0) drawSprite(498, frame, x, y);
    }
    // Walls and doors: argument2 (top), argument4 (bottom), argument5 (left), argument3 (right).
    if (strcmp(c[2], "1") == 0) drawSprite(493, 0, x, y);
    if (strcmp(c[4], "1") == 0) drawSprite(493, 0, x, y + 7);
    if (strcmp(c[5], "1") == 0) drawSprite(494, 0, x, y);
    if (strcmp(c[3], "1") == 0) drawSprite(494, 0, x + 7, y);
    if (strcmp(c[2], "2") == 0) drawSprite(495, 0, x, y);
    if (strcmp(c[4], "2") == 0) drawSprite(495, 0, x, y + 7);
    if (strcmp(c[5], "2") == 0) drawSprite(496, 0, x, y);
    if (strcmp(c[3], "2") == 0) drawSprite(496, 0, x + 7, y);
    // argument7 (c6): the room's item / marker, sprite 491.
    const char* s7 = c[7];
    if (strcmp(s7, "1") == 0) drawSprite(491, 0, x, y);
    if (strcmp(s7, "2") == 0) drawSprite(491, 1, x, y);
    if (strcmp(s7, "3") == 0 && gmlCmp(dm, 1) == 0) drawSprite(491, 2, x, y);
    if (strcmp(s7, "3") == 0 && gmlCmp(dm, 2) == 0) drawSprite(491, 5, x, y);
    if (strcmp(s7, "4") == 0 && gmlCmp(dm, 1) == 0) drawSprite(491, 2, x, y);
    if (strcmp(s7, "4") == 0 && gmlCmp(dm, 2) == 0) drawSprite(491, 3, x, y);
    if (strcmp(s7, "5") == 0) drawSprite(491, 4, x, y);
    if (strcmp(s7, "U") == 0) drawSprite(491, 8, x, y);
    if (strcmp(s7, "D") == 0) drawSprite(491, 9, x, y);
    if (strcmp(s7, "L") == 0) drawSprite(491, 10, x, y);
    if (strcmp(s7, "R") == 0) drawSprite(491, 11, x, y);
    if (strcmp(s7, "H") == 0) drawSprite(491, 12, x, y);
    if (strcmp(s7, "V") == 0) drawSprite(491, 13, x, y);
    if (strcmp(s7, "C") == 0) drawSprite(491, 14, x, y);
    if (gmlCmp(dm, 10) == 0) drawSprite(491, 6, x, y);
    if (gmlCmp(dm, 11) == 0) drawSprite(491, 7, x, y);
}

static RValue native_draw_mapblock(MAYBE_UNUSED VMContext* ctx, RValue* args, int32_t argCount) {
    GMLReal x, y, dm;
    const char* c[10] = { 0 };
    bool ok = argCount >= 10 && argNum(args, argCount, 0, &x) && argNum(args, argCount, 1, &y) && argNum(args, argCount, 8, &dm);
    for (int32_t k = 2; ok && k < 10; k++) {
        if (k == 8) continue;
        ok = args[k].type == RVALUE_STRING && args[k].string != NULL;
        if (ok) c[k] = args[k].string;
    }
    if (!ok) return callOriginal(gCode_draw_mapblock, args, argCount);
    mapBlock(x, y, c, dm);
    return R(0.0);
}

// The cell offsets draw_gui_map uses for map cells m1..m15 (x, y from its arguments).
static const int8_t kMapCellOffset[16][2] = {
    { 0, 0 }, { 16, 12 }, { 0, 4 }, { 8, 4 }, { 16, 4 }, { 24, 4 }, { 32, 4 }, { 0, 12 }, { 8, 12 }, { 24, 12 },
    { 32, 12 }, { 0, 20 }, { 8, 20 }, { 16, 20 }, { 24, 20 }, { 32, 20 },
};

// draw_gui_map(x, y): the HUD minimap from oControl.m1..m15 (7-character cell strings) and dm1..dm15.
static RValue native_draw_gui_map(VMContext* ctx, RValue* args, int32_t argCount) {
    GMLReal x, y, malpha, moffx, moffy, mapmarker;
    Instance* control = VM_findInstanceByTarget(ctx, N.objControl);
    const char* m[16] = { 0 };
    GMLReal dm[16] = { 0 };
    bool ok = control != NULL && argNum(args, argCount, 0, &x) && argNum(args, argCount, 1, &y) &&
        num(control, &v_malpha, &malpha) && num(globalScope(), &v_mapmarker, &mapmarker);
    for (int32_t k = 1; ok && k <= 15; k++) {
        m[k] = str(control, &v_m[k]);
        ok = m[k] != NULL && isAscii(m[k]);
        // dm is only read for cells that are drawn.
        if (ok && strcmp(m[k], "0") != 0) ok = num(control, &v_dm[k], &dm[k]);
    }
    if (ok && truthy(mapmarker)) ok = num(control, &v_moffx, &moffx) && num(control, &v_moffy, &moffy);
    RValue* markfrSlot = ok && truthy(mapmarker) ? varSlot(control, &v_markfr) : NULL;
    if (ok && truthy(mapmarker) && (markfrSlot == NULL || !isNumber(markfrSlot))) ok = false;
    if (!ok) return callOriginal(gCode_draw_gui_map, args, argCount);

    RValue bg[3] = { R(130), R(x), R(y + 4) };
    callVoid(N.drawBackground, bg, 3);
    for (int32_t k = 1; k <= 15; k++) {
        if (strcmp(m[k], "0") == 0) continue;
        // string_char_at(m, 1..7); "" past the end.
        static const char kEmpty[1] = "";
        char chars[8][2];
        const char* c[10] = { 0 };
        int32_t len = (int32_t) strlen(m[k]);
        for (int32_t j = 1; j <= 7; j++) {
            chars[j][0] = j <= len ? m[k][j - 1] : '\0';
            chars[j][1] = '\0';
        }
        // draw_mapblock(x, y, c1, c2, c3, c4, c5, c6, dm, c7)
        for (int32_t j = 1; j <= 6; j++) c[j + 1] = chars[j][0] != '\0' ? chars[j] : kEmpty;
        c[9] = chars[7][0] != '\0' ? chars[7] : kEmpty;
        mapBlock(x + kMapCellOffset[k][0], y + kMapCellOffset[k][1], c, dm[k]);
    }
    // Samus's cell, without touching the surface's alpha.
    RValue cw[4] = { R(1), R(1), R(1), R(0) };
    callVoid(N.drawSetColourWriteEnable, cw, 4);
    drawSpriteExt(497, -1, x + 16, y + 12, 1, 1, 0, -1, malpha);
    RValue cwOn[4] = { R(1), R(1), R(1), R(1) };
    callVoid(N.drawSetColourWriteEnable, cwOn, 4);
    if (!truthy(mapmarker)) return R(0.0);

    GMLReal mf = RValue_toReal(*markfrSlot);
    bool inX = gmlCmp(moffx, -2) >= 0 && gmlCmp(moffx, 2) <= 0;
    bool inY = gmlCmp(moffy, -1) >= 0 && gmlCmp(moffy, 1) <= 0;
    if (inX && inY) drawSprite(500, mf, x + 16 + moffx * 8, y + 12 + moffy * 8);
    if (gmlCmp(moffx, 2) > 0 && inY) drawSprite(502, mf, x + 32, y + 12 + moffy * 8);
    if (gmlCmp(moffx, -2) < 0 && inY) drawSpriteExt(502, mf, x + 8, y + 12 + moffy * 8, -1, 1, 0, -1, 1);
    if (gmlCmp(moffy, -1) < 0 && inX) drawSpriteExt(502, mf, x + 24 + moffx * 8, y + 12, -1, 1, -90, -1, 1);
    if (gmlCmp(moffy, 1) > 0 && inX) drawSpriteExt(502, mf, x + 16 + moffx * 8, y + 20, -1, 1, 90, -1, 1);
    if (gmlCmp(moffy, -1) < 0 && gmlCmp(moffx, 2) > 0) drawSprite(503, mf, x + 32, y + 4);
    if (gmlCmp(moffy, -1) < 0 && gmlCmp(moffx, -2) < 0) drawSpriteExt(503, mf, x + 8, y + 4, -1, 1, 0, -1, 1);
    if (gmlCmp(moffy, 1) > 0 && gmlCmp(moffx, 2) > 0) drawSpriteExt(503, mf, x + 32, y + 28, 1, -1, 0, -1, 1);
    if (gmlCmp(moffy, 1) > 0 && gmlCmp(moffx, -2) < 0) drawSpriteExt(503, mf, x + 8, y + 28, -1, -1, 0, -1, 1);
    return R(0.0);
}

#include "n3ds_am2r_native_hud.inc"

// ===[ Events ]===

// oA6Dust Draw: draw_sprite_ext(sprite_index, -1, x, y, image_xscale, image_yscale, 0, -1, image_alpha - random(0.5))
static bool native_oA6Dust_Draw_0(MAYBE_UNUSED Runner* runner, Instance* self) {
    GMLReal alpha = self->imageAlpha;
    RValue ra[1] = { R(0.5) };
    RValue r = N.random(N.vm, ra, 1);
    alpha -= RValue_toReal(r);
    RValue_free(&r);
    drawSpriteExt(self->spriteIndex, -1, self->x, self->y, self->imageXscale, self->imageYscale, 0, -1, alpha);
    return true;
}

// oEnemy Draw (every enemy's): the sprite, an additive flash while thawing, a grey copy plus three additive ones
// while hit.
static bool native_oEnemy_Draw_0(MAYBE_UNUSED Runner* runner, Instance* self) {
    GMLReal flipx, frozen, flashing, fxtimer = 0, freezetime = 0;
    RValue* frozensprSlot = varSlot(self, &v_frozenspr);
    if (!num(self, &v_flipx, &flipx) || !num(self, &v_frozen, &frozen) || !num(self, &v_flashing, &flashing)) return false;
    GMLReal facing = 0;
    if (truthy(flipx) && !num(self, &v_facing, &facing)) return false;
    if (truthy(frozen) && frozensprSlot == NULL) return false;
    bool thawFlash = !truthy(flashing) && gmlCmp(frozen, 0) > 0;
    if ((thawFlash || truthy(flashing)) && !num(self, &v_fxtimer, &fxtimer)) return false;
    if (thawFlash && !num(self, &v_freezetime, &freezetime)) return false;

    // image_xscale = flipx ? facing : 1 (the VM's setter: it can move the instance in the collision grid).
    VMBuiltins_setVariable(N.vm, self, b_image_xscale, "image_xscale", R(truthy(flipx) ? facing : 1.0), -1);
    // myspr = frozen ? frozenspr : sprite_index
    if (truthy(frozen)) Instance_setSelfVar(self, v_myspr.id, *frozensprSlot);
    else Instance_setSelfVar(self, v_myspr.id, R((GMLReal) self->spriteIndex));
    RValue* mysprSlot = varSlot(self, &v_myspr);
    if (mysprSlot == NULL) return true;
    RValue spr = *mysprSlot;
    GMLReal xs = self->imageXscale, angle = self->imageAngle;

    if (!truthy(flashing)) {
        drawSpriteExtValue(spr, -1, self->x, self->y, xs, 1, angle, -1, self->imageAlpha);
        if (gmlCmp(frozen, 0) > 0 && gmlCmp(frozen, freezetime * 0.2) < 0) {
            blendMode(1);
            drawSpriteExtValue(spr, -1, self->x, self->y, xs, 1, angle, -1, 1 - fxtimer * 0.25);
            blendMode(0);
        }
    }
    if (truthy(flashing)) {
        // make_color_rgb(80, 80, 80)
        drawSpriteExtValue(spr, -1, self->x, self->y, xs, 1, angle, 80 + 80 * 256 + 80 * 65536, 1);
        blendMode(1);
        for (int32_t k = 0; k < 3; k++) drawSpriteExtValue(spr, -1, self->x, self->y, xs, 1, angle, -1, 1 - fxtimer * 0.25);
        blendMode(0);
    }
    return true;
}

// ===[ Table ]===

static NativeEntry gEntries[] = {
    { "approximatelyZero", native_approximatelyZero, NULL, 0, -1, false },
    { "calculateCollisionBounds", native_calculateCollisionBounds, NULL, 0, -1, false },
    { "isCollisionLeft", native_isCollisionLeft, NULL, 0, -1, false },
    { "isCollisionRight", native_isCollisionRight, NULL, 0, -1, false },
    { "isCollisionTop", native_isCollisionTop, NULL, 0, -1, false },
    { "isCollisionBottom", native_isCollisionBottom, NULL, 0, -1, false },
    { "isCollisionRectangle", native_isCollisionRectangle, NULL, 0, -1, false },
    { "string_split", native_string_split, NULL, 0, -1, false },
    { "draw_mapblock", native_draw_mapblock, NULL, 0, -1, false },
    { "draw_gui_map", native_draw_gui_map, NULL, 0, -1, false },
    { "gml_Object_oA6Dust_Draw_0", NULL, native_oA6Dust_Draw_0, 0, -1, false },
    { "gml_Object_oEnemy_Draw_0", NULL, native_oEnemy_Draw_0, 0, -1, false },
    { "draw_gui", native_draw_gui, NULL, 0, -1, false },
    { "gml_Object_oLightEngine_Other_11", NULL, native_oLightEngine_Other_11, 0, -1, false },
};
#define N_ENTRIES ((int32_t) (sizeof(gEntries) / sizeof(gEntries[0])))

// The bytecode each native was written from (AM2R 1.5.5).
#include "n3ds_am2r_native_hashes.h"

static uint32_t codeHash(int32_t codeIndex) {
    DataWin* dw = N.runner->dataWin;
    CodeEntry* code = &dw->code.entries[codeIndex];
    const uint8_t* base = dw->bytecodeBuffer + (code->bytecodeAbsoluteOffset - dw->bytecodeBufferBase);
    uint32_t h = 2166136261u;
    for (uint32_t k = code->offset; k < code->length; k++) h = (h ^ base[k]) * 16777619u;
    return h ^ code->length;
}

static int32_t codeIndexByName(const char* name) {
    DataWin* dw = N.runner->dataWin;
    for (uint32_t k = 0; k < dw->code.count; k++) {
        if (dw->code.entries[k].name != NULL && strcmp(dw->code.entries[k].name, name) == 0) return (int32_t) k;
    }
    return -1;
}

static void install(bool on) {
    for (ptrdiff_t k = 0; k < arrlen(N.hooks); k++) {
        NativeEntry* e = &gEntries[N.hooks[k].entry];
        N.vm->funcCallCache[N.hooks[k].funcIndex].builtin = on ? (void*) e->script : N.hooks[k].original;
    }
    N.runner->nativeEventCode = on ? N.events : NULL;
}

void N3DSAm2rNative_init(Runner* runner) {
    N.runner = runner;
    N.vm = runner->vmContext;
    N.objSolid = N3DSGml_objectIndex("oSolid");
    N.objControl = N3DSGml_objectIndex("oControl");
    N.collisionLine = builtin("collision_line");
    N.drawSprite = builtin("draw_sprite");
    N.drawSpriteExt = builtin("draw_sprite_ext");
    N.drawBackground = builtin("draw_background");
    N.drawSetBlendMode = builtin("draw_set_blend_mode");
    N.drawSetColourWriteEnable = builtin("draw_set_colour_write_enable");
    N.random = builtin("random");
    N.drawText = builtin("draw_text");
    N.drawTextColor = builtin("draw_text_color");
    N.drawSetFont = builtin("draw_set_font");
    N.drawSetAlpha = builtin("draw_set_alpha");
    N.drawSetColor = builtin("draw_set_color");
    N.string = builtin("string");
    N.makeColorRgb = builtin("make_color_rgb");
    N.makeColorHsv = builtin("make_color_hsv");
    N.surfaceSetTarget = builtin("surface_set_target");
    N.surfaceResetTarget = builtin("surface_reset_target");
    N.drawClear = builtin("draw_clear");
    N.drawRectangle = builtin("draw_rectangle");
    N.drawBackgroundExt = builtin("draw_background_ext");
    N.objCharacter = N3DSGml_objectIndex("oCharacter");
    gObjLight = N3DSGml_objectIndex("oLight");
    gObjBeam = N3DSGml_objectIndex("oBeam");
    gObjMissile = N3DSGml_objectIndex("oMissile");
    gObjBomb = N3DSGml_objectIndex("oBomb");
    gObjBomb2 = N3DSGml_objectIndex("oBomb2");
    gObjPickup = N3DSGml_objectIndex("oPickup");
    gObjMGammaElec = N3DSGml_objectIndex("oMGammaElec");
    gObjGlowPlant1 = N3DSGml_objectIndex("oGlowPlant1");
    gObjSpikePlant = N3DSGml_objectIndex("oSpikePlant");
    gObjPincherFly = N3DSGml_objectIndex("oPincherFly");
    gObjA3LabLight = N3DSGml_objectIndex("oA3LabLight");
    gObjA3LabDoor = N3DSGml_objectIndex("oA3LabDoor");
    gObjFXAnimSpark = N3DSGml_objectIndex("oFXAnimSpark");
    gObjLightBug = N3DSGml_objectIndex("oLightBug");
    gObjChargeBeamSpark1 = N3DSGml_objectIndex("oChargeBeamSpark1");
    gObjItemBall = N3DSGml_objectIndex("oItemBall");
    gObjItem = N3DSGml_objectIndex("oItem");
    gObjDoor = N3DSGml_objectIndex("oDoor");
    gObjMOmegaFlame = N3DSGml_objectIndex("oMOmegaFlame");
    gObjMOmegaProjectile = N3DSGml_objectIndex("oMOmega_Projectile");
    gObjA8Lamp = N3DSGml_objectIndex("oA8Lamp");
    gObjA8RedLight = N3DSGml_objectIndex("oA8RedLight");
    gObjA6Dust = N3DSGml_objectIndex("oA6Dust");
    gObjA8RedLightFX = N3DSGml_objectIndex("oA8RedLightFX");
    gObjGenesisAcid = N3DSGml_objectIndex("oGenesisAcid");
    gObjGenesisSlashProj = N3DSGml_objectIndex("oGenesisSlashProj");
    gObjElderSeptogg = N3DSGml_objectIndex("oElderSeptogg");
    gObjXPickup = N3DSGml_objectIndex("oXPickup");
    gObjCoreX = N3DSGml_objectIndex("oCoreX");
    b_image_xscale = VMBuiltins_resolveBuiltinVarId("image_xscale");
    b_image_blend = VMBuiltins_resolveBuiltinVarId("image_blend");
    b_view_xview = VMBuiltins_resolveBuiltinVarId("view_xview");
    b_view_yview = VMBuiltins_resolveBuiltinVarId("view_yview");
    if (N.objSolid < 0 || N.objControl < 0 || N.collisionLine == NULL || N.drawSprite == NULL || N.drawSpriteExt == NULL ||
        N.drawBackground == NULL || N.drawSetBlendMode == NULL || N.drawSetColourWriteEnable == NULL || N.random == NULL ||
        N.drawText == NULL || N.drawTextColor == NULL || N.drawSetFont == NULL || N.drawSetAlpha == NULL ||
        N.drawSetColor == NULL || N.string == NULL || N.makeColorRgb == NULL || N.makeColorHsv == NULL ||
        N.surfaceSetTarget == NULL || N.surfaceResetTarget == NULL || N.drawClear == NULL || N.drawRectangle == NULL ||
        N.drawBackgroundExt == NULL || N.objCharacter < 0 || gObjLight < 0 || gObjBeam < 0 || gObjMissile < 0 || gObjBomb < 0 || gObjBomb2 < 0 || gObjPickup < 0 || gObjMGammaElec < 0 || gObjGlowPlant1 < 0 || gObjSpikePlant < 0 || gObjPincherFly < 0 || gObjA3LabLight < 0 || gObjA3LabDoor < 0 || gObjFXAnimSpark < 0 || gObjLightBug < 0 || gObjChargeBeamSpark1 < 0 || gObjItemBall < 0 || gObjItem < 0 || gObjDoor < 0 || gObjMOmegaFlame < 0 || gObjMOmegaProjectile < 0 || gObjA8Lamp < 0 || gObjA8RedLight < 0 || gObjA6Dust < 0 || gObjA8RedLightFX < 0 || gObjGenesisAcid < 0 || gObjGenesisSlashProj < 0 || gObjElderSeptogg < 0 || gObjXPickup < 0 || gObjCoreX < 0 ||
        b_image_xscale < 0 || b_image_blend < 0 || b_view_xview < 0 || b_view_yview < 0) {
        logInfo("AM2R natives: not this game\n");
        return;
    }

    Var* vars[] = { &v_lb, &v_tb, &v_rb, &v_bb, &v_collisionBoundsOffsetLeftX, &v_collisionBoundsOffsetTopY,
        &v_collisionBoundsOffsetRightX, &v_collisionBoundsOffsetBottomY, &v_i, &v_w1, &v_h1, &v_w2, &v_h2, &v_flipx,
        &v_facing, &v_frozen, &v_frozenspr, &v_myspr, &v_flashing, &v_freezetime, &v_fxtimer, &v_malpha, &v_moffx,
        &v_moffy, &v_markfr, &v_mapmarker, &v_guifont1, &v_guifont1a, &v_guifont2, &v_missiles, &v_smissiles, &v_pbombs, &v_classicmode, &v_opshowhud, &v_etanks, &v_playerhealth, &v_maxmissiles, &v_opmslstyle, &v_currentweapon, &v_maxsmissiles, &v_maxpbombs, &v_ophudshowmap, &v_ophudshowmetrcount, &v_mod_etankhealthmult, &v_hudflash, &v_hudflashfx, &v_widescreen_space, &v_state, &v_sjball, &v_armmsl, &v_xoff, &v_etankxoff, &v_monstersarea, &v_monstersleft, &v_widescreen, &v_surf, &v_scale, &v_flying_sprite, &v_yy, &v_xtype };
    for (size_t k = 0; k < sizeof(vars) / sizeof(vars[0]); k++) resolveVar(vars[k]);
    for (int32_t k = 1; k <= 15; k++) {
        snprintf(gMapVarNames[k], sizeof(gMapVarNames[k]), "m%d", (int) k);
        snprintf(gMapVarNames[16 + k], sizeof(gMapVarNames[16 + k]), "dm%d", (int) k);
        v_m[k].name = gMapVarNames[k];
        v_dm[k].name = gMapVarNames[16 + k];
        resolveVar(&v_m[k]);
        resolveVar(&v_dm[k]);
    }

    N.events = (NativeEventCode*) safeCalloc(runner->dataWin->code.count > 0 ? runner->dataWin->code.count : 1, sizeof(NativeEventCode));
    for (int32_t k = 0; k < N_ENTRIES; k++) {
        NativeEntry* e = &gEntries[k];
        e->code = e->script != NULL ? N3DSGml_scriptIndex(e->name) : codeIndexByName(e->name);
        if (e->code < 0) continue;
        uint32_t hash = codeHash(e->code);
        uint32_t expected = N3DSAm2rNative_expectedHash(e->name);
        e->matches = expected != 0 && hash == expected;
        if (!e->matches) {
            logInfo("AM2R natives: %s left to the game's code (bytecode %08lx, written for %08lx)\n", e->name,
                (unsigned long) hash, (unsigned long) expected);
            continue;
        }
        N.available++;
        if (e->event != NULL) {
            N.events[e->code] = e->event;
            continue;
        }
        DataWin* dw = runner->dataWin;
        for (uint32_t f = 0; f < N.vm->funcCallCacheCount; f++) {
            if (dw->func.functions[f].name == NULL || strcmp(dw->func.functions[f].name, e->name) != 0) continue;
            if (N.vm->funcCallCache[f].scriptCodeIndex != e->code) continue;
            arrput(N.hooks, ((typeof(*N.hooks)) { (int32_t) f, k, N.vm->funcCallCache[f].builtin }));
        }
    }
    // Remember these while the game passes them around: the per-script code indices for the fallbacks.
    gCode_approximatelyZero = gEntries[0].code;
    gCode_calculateCollisionBounds = gEntries[1].code;
    gCode_isCollisionLeft = gEntries[2].code;
    gCode_isCollisionRight = gEntries[3].code;
    gCode_isCollisionTop = gEntries[4].code;
    gCode_isCollisionBottom = gEntries[5].code;
    gCode_isCollisionRectangle = gEntries[6].code;
    gCode_string_split = gEntries[7].code;
    gCode_draw_mapblock = gEntries[8].code;
    gCode_draw_gui_map = gEntries[9].code;
    gCode_draw_gui = gEntries[12].code;
    gCode_gui_health = N3DSGml_scriptIndex("gui_health");
    gCode_to_string_lz = N3DSGml_scriptIndex("to_string_lz");
    if (gCode_gui_health < 0 || gCode_to_string_lz < 0) {
        // draw_gui calls these; without them it stays the game's code.
        gEntries[12].matches = false;
        for (ptrdiff_t k = arrlen(N.hooks) - 1; k >= 0; k--) {
            if (N.hooks[k].entry == 12) arrdel(N.hooks, k);
        }
    }
    N.initialised = true;
    logInfo("AM2R natives: %ld of %ld match this game\n", (long) N.available, (long) N_ENTRIES);
    install(N.enabled);
}

void N3DSAm2rNative_setEnabled(bool enabled) {
    N.enabled = enabled;
    if (N.initialised) install(enabled);
}

bool N3DSAm2rNative_enabled(void) {
    return N.enabled;
}

int32_t N3DSAm2rNative_available(void) {
    return N.available;
}
