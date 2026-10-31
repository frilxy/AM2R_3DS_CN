#pragma once

#include "../renderer.h"

#include <citro3d.h>

#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif

Renderer* N3DSRenderer_create(void);
bool N3DSRenderer_isReady(Renderer* renderer);
const char* N3DSRenderer_getStartupError(Renderer* renderer);
void N3DSRenderer_beginOverlay(Renderer* renderer);
void N3DSRenderer_beginBottomScreenGUIEx(Renderer* renderer, int32_t guiW, int32_t guiH, float scaleX, float scaleY, float offsetX, float offsetY);
void N3DSRenderer_beginBottomScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endBottomScreenGUI(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI(Renderer* renderer);
void N3DSRenderer_beginBottomScreenGUI2x(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endBottomScreenGUI2x(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI(Renderer* renderer);
void N3DSRenderer_beginTopScreenGUI2x(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_endTopScreenGUI2x(Renderer* renderer);
bool N3DSRenderer_isTopScreenGUIActive(Renderer* renderer);
bool N3DSRenderer_isTopScreenBattleViewActive(Renderer* renderer);
void N3DSRenderer_setTopScreenBattleViewActive(Renderer* renderer, bool active);
uint32_t N3DSRenderer_getResidentAtlasVRAMBytes(Renderer* renderer);
uint32_t N3DSRenderer_getResidentAtlasVRAMLimitBytes(Renderer* renderer);
uint32_t N3DSRenderer_getResidentAtlasPageCount(Renderer* renderer);
uint32_t N3DSRenderer_getResidentAtlasPageLimit(Renderer* renderer);
uint32_t N3DSRenderer_getResidentDirectAssetVRAMBytes(Renderer* renderer);
uint32_t N3DSRenderer_getResidentDirectAssetVRAMLimitBytes(Renderer* renderer);
uint32_t N3DSRenderer_getFrameFragmentDraws(Renderer* renderer);
uint32_t N3DSRenderer_getFrameSpriteDrawCalls(Renderer* renderer);
uint32_t N3DSRenderer_getFrameSpritePartDrawCalls(Renderer* renderer);
uint32_t N3DSRenderer_getFrameDirectSpriteHits(Renderer* renderer);
uint32_t N3DSRenderer_getFrameDirectAssetLoads(Renderer* renderer);
uint32_t N3DSRenderer_getFrameTextureSwitches(Renderer* renderer);
uint32_t N3DSRenderer_getFrameTextGlyphDraws(Renderer* renderer);
uint32_t N3DSRenderer_getFrameTextTenthsMs(Renderer* renderer);
int32_t N3DSRenderer_findTileEntryIndex(Renderer* renderer, int32_t backgroundIndex, int32_t srcX, int32_t srcY, int32_t srcW, int32_t srcH);
bool N3DSRenderer_drawCachedTileEntry(Renderer* renderer, int32_t tileEntryIndex, float drawX, float drawY, float xscale, float yscale, uint32_t color, float alpha);
// Loads the textures a room is likely to need (call when the room changes).
void N3DSRenderer_prewarmRoom(Renderer* renderer, Runner* runner);
void N3DSRenderer_importReadAhead(Renderer* renderer);
// The frame gate (see n3ds_renderer.c): frameGate opens the GPU frame if it isn't open (C3D_FrameBegin: waits for the
// previous frame and the vblank); frameEnd submits it.
void N3DSRenderer_frameGate(void);
void N3DSRenderer_frameEnd(void);
bool N3DSRenderer_frameIsOpen(void);
double N3DSRenderer_frameWaitMs(void);
double N3DSRenderer_frameGpuMs(void);
C3D_RenderTarget* N3DSRenderer_getTopTarget(Renderer* renderer);
C3D_RenderTarget* N3DSRenderer_getBottomTarget(Renderer* renderer);
// Deletes surfaces freed during earlier frames. Call outside C3D_FrameBegin/End; all = everything (shutdown).
void N3DSRenderer_collectGarbage(Renderer* renderer, bool all);
void N3DSRenderer_logDiag(Renderer* renderer);
// Stretch the game to the whole top screen (else integer scale, centred).
void N3DSRenderer_setStretchToScreen(Renderer* renderer, bool stretch);
// 1x / 2x screen modes: the game's picture at this scale on the top screen, centred (0: the usual fit).
void N3DSRenderer_setFixedScale(Renderer* renderer, int32_t scale);
// Sends the game's screen (RENDER_TARGET_HOST_FRAMEBUFFER) to the bottom screen, 1:1 (the pause screen).
void N3DSRenderer_setHostScreenBottom(Renderer* renderer, bool bottom);
// Keeps the last finished top-screen frame (call right after C3D_FrameBegin) / shows it / lets it go.
bool N3DSRenderer_captureFrozenTop(Renderer* renderer);
void N3DSRenderer_drawFrozenTop(Renderer* renderer);
void N3DSRenderer_dropFrozenTop(Renderer* renderer);
// Keeps the finished bottom screen (the pause screen; call right after C3D_FrameBegin) / shows it.
bool N3DSRenderer_captureBottomSnapshot(Renderer* renderer);
bool N3DSRenderer_hasBottomSnapshot(Renderer* renderer);
void N3DSRenderer_drawBottomSnapshot(Renderer* renderer);
// Plain citro2d drawing in screen pixels on top of the game's frame (top or bottom screen).
#define N3DS_OVERLAY_DEPTH 1.0f
void N3DSRenderer_beginScreenOverlay(Renderer* renderer, bool top);
void N3DSRenderer_endScreenOverlay(Renderer* renderer);
