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
// Runtime textures for font_replace and background_replace (see docs/localization.md). Every font size shares ONE
// texture: with two or more, particular strings stopped drawing while the atlases themselves looked correct.
int32_t N3DSRenderer_allocSharedFontAtlas(Renderer* renderer);
int32_t N3DSRenderer_sharedFontAtlasIndex(Renderer* renderer);
int32_t N3DSRenderer_sharedFontAtlasTPAG(Renderer* renderer);
bool N3DSRenderer_uploadFontAtlasRegion(Renderer* renderer, const uint8_t* rgba, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
// The shared texture's first free row. Each font size stacks below the previous one, and reallocating the atlas
// resets it, so the two are kept together.
uint32_t N3DSRenderer_sharedFontAtlasNextY(Renderer* renderer);
void N3DSRenderer_setSharedFontAtlasNextY(Renderer* renderer, uint32_t y);
int32_t N3DSRenderer_allocRuntimeImage(Renderer* renderer, const uint8_t* rgba, uint32_t w, uint32_t h);
// Optional 320x240 art drawn on the bottom screen wherever the port would leave it black (title, file select,
// options, pause). Stretched to the screen, so any size works; 320x240 is what it is designed for.
bool N3DSRenderer_setBottomBackground(Renderer* renderer, const uint8_t* rgba, uint32_t w, uint32_t h);
bool N3DSRenderer_setRuntimeTPAGContent(Renderer* renderer, int32_t tpagIndex, uint32_t contentW, uint32_t contentH);
// A swapped font has a whole new glyph array; a text layout cached before the swap still holds the old rectangles.
void N3DSRenderer_invalidateTextLayoutCache(Renderer* renderer);
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
// Draws the game's GUI on the bottom screen: everything drawn between begin and end is mapped into a guiW x guiH box
// fitted to the 320x240 screen, then the top screen's own mapping is restored. Ex takes a scale and an offset.
void N3DSRenderer_beginBottomScreenGUI(Renderer* renderer, int32_t guiW, int32_t guiH);
void N3DSRenderer_beginBottomScreenGUIEx(Renderer* renderer, int32_t guiW, int32_t guiH, float scaleX, float scaleY, float offsetX, float offsetY);
void N3DSRenderer_endBottomScreenGUI(Renderer* renderer);
// The bottom screen's static layer, cached in a texture the port owns. Most of the map (its cells, bars, zoom labels
// and title) only changes when the map is panned, zoomed or newly explored, so it is drawn into this cache once and
// blitted back on later frames; the counters, the clock and the pulsing marker stay per-frame and are drawn on top.
// Call invalidate whenever the static layer would change, then draw it into the cache with begin/end (the caller
// draws exactly what it would draw to the bottom screen), and blit it inside a bottom-screen GUI pass.
void N3DSRenderer_invalidateBottomCache(Renderer* renderer);
bool N3DSRenderer_beginBottomCacheDraw(Renderer* renderer);
void N3DSRenderer_endBottomCacheDraw(Renderer* renderer);
void N3DSRenderer_blitBottomCache(Renderer* renderer);
void N3DSRenderer_freeBottomCache(Renderer* renderer);
// Deletes surfaces freed during earlier frames. Call outside C3D_FrameBegin/End; all = everything (shutdown).
void N3DSRenderer_collectGarbage(Renderer* renderer, bool all);
void N3DSRenderer_logDiag(Renderer* renderer);
// Stretch the game to the whole top screen (else integer scale, centred).
void N3DSRenderer_setStretchToScreen(Renderer* renderer, bool stretch);
// 1x / 2x screen modes: the game's picture at this scale on the top screen, centred (0: the usual fit).
void N3DSRenderer_setFixedScale(Renderer* renderer, int32_t scale);
// Plain citro2d drawing in screen pixels on top of the game's frame, on the top screen.
#define N3DS_OVERLAY_DEPTH 1.0f
void N3DSRenderer_beginScreenOverlay(Renderer* renderer);
void N3DSRenderer_endScreenOverlay(Renderer* renderer);
