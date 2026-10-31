#include "n3ds_gml.h"

#include "../gml_array.h"
#include "../instance.h"
#include "../int_rvalue_hashmap.h"
#include "../runner.h"
#include "../utils.h"
#include "../vm.h"

#include <stb/ds/stb_ds.h>
#include <string.h>

static Runner* gRunner = NULL;

void N3DSGml_init(Runner* runner) {
    gRunner = runner;
}

int32_t N3DSGml_objectIndex(const char* name) {
    if (gRunner == NULL) return -1;
    Objt* objt = &gRunner->dataWin->objt;
    for (uint32_t i = 0; i < objt->count; i++) {
        if (objt->objects[i].name != NULL && strcmp(objt->objects[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

int32_t N3DSGml_roomIndex(const char* name) {
    if (gRunner == NULL) return -1;
    for (uint32_t i = 0; i < gRunner->dataWin->room.count; i++) {
        if (gRunner->dataWin->room.rooms[i].name != NULL && strcmp(gRunner->dataWin->room.rooms[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

int32_t N3DSGml_instances(int32_t objectIndex, Instance** out, int32_t max) {
    if (gRunner == NULL || objectIndex < 0) return 0;
    int32_t n = 0;
    for (ptrdiff_t i = 0; i < arrlen(gRunner->instances) && n < max; i++) {
        Instance* inst = gRunner->instances[i];
        if (inst == NULL || inst->destroyed || !inst->active || inst->objectIndex != objectIndex) continue;
        out[n++] = inst;
    }
    return n;
}

Instance* N3DSGml_anyInstance(int32_t objectIndex) {
    if (gRunner == NULL || objectIndex < 0) return NULL;
    for (ptrdiff_t i = 0; i < arrlen(gRunner->instances); i++) {
        Instance* inst = gRunner->instances[i];
        if (inst != NULL && !inst->destroyed && inst->objectIndex == objectIndex) return inst;
    }
    return NULL;
}

Instance* N3DSGml_firstInstance(int32_t objectIndex) {
    Instance* inst = NULL;
    return N3DSGml_instances(objectIndex, &inst, 1) == 1 ? inst : NULL;
}

int32_t N3DSGml_spriteIndex(const char* name) {
    if (gRunner == NULL) return -1;
    Sprt* sprt = &gRunner->dataWin->sprt;
    for (uint32_t i = 0; i < sprt->count; i++) {
        if (sprt->sprites[i].name != NULL && strcmp(sprt->sprites[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

int32_t N3DSGml_backgroundIndex(const char* name) {
    if (gRunner == NULL) return -1;
    Bgnd* bgnd = &gRunner->dataWin->bgnd;
    for (uint32_t i = 0; i < bgnd->count; i++) {
        if (bgnd->backgrounds[i].name != NULL && strcmp(bgnd->backgrounds[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

int32_t N3DSGml_fontIndex(const char* name) {
    if (gRunner == NULL) return -1;
    FontChunk* font = &gRunner->dataWin->font;
    for (uint32_t i = 0; i < font->count; i++) {
        if (font->fonts[i].name != NULL && strcmp(font->fonts[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

int32_t N3DSGml_soundIndex(const char* name) {
    if (gRunner == NULL) return -1;
    Sond* sond = &gRunner->dataWin->sond;
    for (uint32_t i = 0; i < sond->count; i++) {
        if (sond->sounds[i].name != NULL && strcmp(sond->sounds[i].name, name) == 0) return (int32_t) i;
    }
    return -1;
}

int32_t N3DSGml_scriptIndex(const char* name) {
    if (gRunner == NULL || gRunner->vmContext == NULL) return -1;
    ptrdiff_t slot = shgeti(gRunner->vmContext->codeIndexByName, (char*) name);
    return slot >= 0 ? gRunner->vmContext->codeIndexByName[slot].value : -1;
}

RValue N3DSGml_callScript(int32_t scriptIndex, RValue* args, int32_t argCount) {
    if (gRunner == NULL || gRunner->vmContext == NULL || scriptIndex < 0) return RValue_makeUndefined();
    return VM_callCodeIndex(gRunner->vmContext, scriptIndex, args, argCount);
}

Instance* N3DSGml_instanceById(int32_t id) {
    if (gRunner == NULL) return NULL;
    Instance* inst = hmget(gRunner->instancesById, id);
    return inst != NULL && !inst->destroyed ? inst : NULL;
}

Instance* N3DSGml_create(float x, float y, int32_t objectIndex) {
    if (gRunner == NULL || objectIndex < 0) return NULL;
    return Runner_createInstance(gRunner, x, y, objectIndex);
}

void N3DSGml_destroy(Instance* inst) {
    if (gRunner == NULL || inst == NULL || inst->destroyed) return;
    Runner_destroyInstance(gRunner, inst, true);
}

static int32_t N3DSGml_varId(const char* name) {
    if (gRunner == NULL || gRunner->vmContext == NULL) return -1;
    ptrdiff_t slot = shgeti(gRunner->vmContext->varNameMap, (char*) name);
    return slot >= 0 ? gRunner->vmContext->varNameMap[slot].value : -1;
}

static bool N3DSGml_readSlot(Instance* inst, const char* name, int32_t index, double* out) {
    int32_t varId = N3DSGml_varId(name);
    if (inst == NULL || varId < 0) return false;
    RValue* slot = IntRValueHashMap_findSlot(&inst->selfVars, varId);
    if (slot == NULL || (index >= 0 && slot->type != RVALUE_ARRAY)) return false;
    RValue value = index >= 0 ? GMLArray_getOnArrayRef(slot, index) : *slot;
    if (value.type != RVALUE_REAL && value.type != RVALUE_INT32 && value.type != RVALUE_INT64 && value.type != RVALUE_BOOL && value.type != RVALUE_ASSETREF) return false;
    *out = (double) RValue_toReal(value);
    return true;
}

RValue* N3DSGml_varSlot(Instance* inst, const char* name) {
    int32_t varId = N3DSGml_varId(name);
    if (inst == NULL || varId < 0) return NULL;
    return IntRValueHashMap_findSlot(&inst->selfVars, varId);
}

const char* N3DSGml_getVarString(Instance* inst, const char* name) {
    RValue* slot = N3DSGml_varSlot(inst, name);
    return slot != NULL && slot->type == RVALUE_STRING ? slot->string : NULL;
}

void N3DSGml_setVarString(Instance* inst, const char* name, const char* value) {
    int32_t varId = N3DSGml_varId(name);
    if (inst == NULL || varId < 0) return;
    Instance_setSelfVar(inst, varId, RValue_makeOwnedString(safeStrdup(value)));
}

void N3DSGml_setGlobalString(const char* name, const char* value) {
    if (gRunner == NULL || gRunner->vmContext == NULL) return;
    N3DSGml_setVarString((Instance*) gRunner->vmContext->globalScopeInstance, name, value);
}

bool N3DSGml_getVarElement(Instance* inst, const char* name, int32_t index, double* out) {
    return N3DSGml_readSlot(inst, name, index, out);
}

void N3DSGml_setVarElement(Instance* inst, const char* name, int32_t index, double value) {
    RValue* slot = N3DSGml_varSlot(inst, name);
    if (slot == NULL || slot->type != RVALUE_ARRAY || index < 0) return;
    GMLArray_setOnArrayRef(slot, index, RValue_makeReal(value));
}

bool N3DSGml_getGlobal(const char* name, int32_t index, double* out) {
    if (gRunner == NULL || gRunner->vmContext == NULL) return false;
    return N3DSGml_readSlot((Instance*) gRunner->vmContext->globalScopeInstance, name, index, out);
}

RValue* N3DSGml_globalSlot(const char* name) {
    if (gRunner == NULL || gRunner->vmContext == NULL || gRunner->vmContext->globalScopeInstance == NULL) return NULL;
    int32_t varId = N3DSGml_varId(name);
    if (varId < 0) return NULL;
    return IntRValueHashMap_findSlot(&((Instance*) gRunner->vmContext->globalScopeInstance)->selfVars, varId);
}

void N3DSGml_setGlobalElement(const char* name, int32_t index, double value) {
    RValue* slot = N3DSGml_globalSlot(name);
    if (slot == NULL || slot->type != RVALUE_ARRAY || index < 0) return;
    GMLArray_setOnArrayRef(slot, index, RValue_makeReal(value));
}

void N3DSGml_setGlobal(const char* name, double value) {
    int32_t varId = N3DSGml_varId(name);
    if (varId < 0 || gRunner->vmContext->globalScopeInstance == NULL) return;
    Instance_setSelfVar((Instance*) gRunner->vmContext->globalScopeInstance, varId, RValue_makeReal(value));
}

bool N3DSGml_getVar(Instance* inst, const char* name, double* out) {
    if (inst == NULL) return false;
    if (strcmp(name, "x") == 0) { *out = inst->x; return true; }
    if (strcmp(name, "y") == 0) { *out = inst->y; return true; }
    return N3DSGml_readSlot(inst, name, -1, out);
}

void N3DSGml_setVar(Instance* inst, const char* name, double value) {
    if (inst == NULL) return;
    if (strcmp(name, "x") == 0) { inst->x = (float) value; return; }
    if (strcmp(name, "y") == 0) { inst->y = (float) value; return; }
    int32_t varId = N3DSGml_varId(name);
    if (varId >= 0) Instance_setSelfVar(inst, varId, RValue_makeReal(value));
}

void N3DSGml_viewOrigin(float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    if (gRunner == NULL) return;
    GMLCamera* camera = Runner_getCameraById(gRunner, gRunner->views[0].cameraId);
    if (camera == NULL) return;
    *x = camera->viewX;
    *y = camera->viewY;
}
