#pragma once

// Reading and writing a running game's variables from the 3DS front end (the pause screen's touch controls).
// Names are resolved once and cached; everything is a no-op returning false/NULL when a name doesn't exist.

#include "../runner.h"

#include <stdbool.h>
#include <stdint.h>

void N3DSGml_init(Runner* runner);

// Index of the object with this name, -1 if none.
int32_t N3DSGml_objectIndex(const char* name);
// Index of the room with this name, -1 if none.
int32_t N3DSGml_roomIndex(const char* name);
// Index of the sprite with this name, -1 if none.
int32_t N3DSGml_spriteIndex(const char* name);
// Index of the background / font / sound with this name, -1 if none.
int32_t N3DSGml_backgroundIndex(const char* name);
int32_t N3DSGml_fontIndex(const char* name);
int32_t N3DSGml_soundIndex(const char* name);
// Code index of the game script with this name, -1 if none.
int32_t N3DSGml_scriptIndex(const char* name);
// Calls a game script; the result is the caller's to free (RValue_free).
RValue N3DSGml_callScript(int32_t scriptIndex, RValue* args, int32_t argCount);

// First active, live instance of an object (by index), NULL if none.
Instance* N3DSGml_firstInstance(int32_t objectIndex);
// First live instance of an object, active or not, NULL if none.
Instance* N3DSGml_anyInstance(int32_t objectIndex);
// Fills out[] with up to max active, live instances of an object; returns how many.
int32_t N3DSGml_instances(int32_t objectIndex, Instance** out, int32_t max);

// global.<name> (or global.<name>[index] with index >= 0) as a number.
bool N3DSGml_getGlobal(const char* name, int32_t index, double* out);
void N3DSGml_setGlobal(const char* name, double value);
// global.<name>[index] = value (the global must already hold an array).
void N3DSGml_setGlobalElement(const char* name, int32_t index, double value);
// The value slot of global.<name> (NULL if it isn't set): read many elements of a global array without a lookup each.
// Valid until the game next assigns that global.
RValue* N3DSGml_globalSlot(const char* name);
// The instance with this id, NULL if none (or destroyed).
Instance* N3DSGml_instanceById(int32_t id);
// Creates an instance (runs its Create event); destroys one (with its Destroy event).
Instance* N3DSGml_create(float x, float y, int32_t objectIndex);
void N3DSGml_destroy(Instance* inst);
// The value slot of <inst>.<name>, NULL if it isn't set.
RValue* N3DSGml_varSlot(Instance* inst, const char* name);
// <inst>.<name> as a string (NULL if it isn't one); <inst>.<name> = a copy of value.
const char* N3DSGml_getVarString(Instance* inst, const char* name);
void N3DSGml_setVarString(Instance* inst, const char* name, const char* value);
void N3DSGml_setGlobalString(const char* name, const char* value);
// <inst>.<name>[index] as a number / = value (the variable must already hold an array).
bool N3DSGml_getVarElement(Instance* inst, const char* name, int32_t index, double* out);
void N3DSGml_setVarElement(Instance* inst, const char* name, int32_t index, double value);
// <inst>.<name> as a number.
bool N3DSGml_getVar(Instance* inst, const char* name, double* out);
void N3DSGml_setVar(Instance* inst, const char* name, double value);

// Top-left of view 0 in room coordinates (where the screen's 0,0 is).
void N3DSGml_viewOrigin(float* x, float* y);
