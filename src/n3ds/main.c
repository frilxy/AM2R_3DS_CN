#include "../data_win.h"
#include "../runner.h"
#include "../runner_keyboard.h"
#include "../utils.h"
#include "../vm.h"

#include "../gettime.h"
#include "../log.h"
#include "../noop_audio_system.h"
#include "../runner_gamepad.h"
#include "../runner_mouse.h"

#include "../input_recording.h"
#include "../overlay_file_system.h"
#include "n3ds_audio_system.h"
#include "n3ds_cached_file_system.h"
#include "n3ds_input.h"
#include "n3ds_am2r.h"
#include "n3ds_font_replace.h"
#include "n3ds_livemap.h"
#include "n3ds_pause.h"
#include "n3ds_platform_config.h"
#include "n3ds_prof.h"
#include "n3ds_renderer.h"
#include "n3ds_screen.h"
#include "n3ds_unimpl.h"

#include <3ds.h>
#include <citro2d.h>

#include "stb_image.h"

#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define N3DS_LOADING_TEXT_SCALE 0.42f
#define N3DS_BOOT_LOG_MAX_LINES 6
#define N3DS_BOOT_LOG_LINE_CHARS 88
#define N3DS_TOTAL_VRAM_BYTES (6u * 1024u * 1024u)
// citro2d drops draws past this many objects per frame; AM2R rooms can draw a few thousand tiles.
#define N3DS_C2D_MAX_OBJECTS 16384
#define N3DS_TOP_SCREEN_W 400
#define N3DS_TOP_SCREEN_H 240

extern u32 __ctru_heap_size;

void N3DSLog_init(void);
void N3DSLog_close(void);
void N3DSLog_tick(void);

typedef struct {
    bool useCitro2D;
    C3D_RenderTarget* target;
    C2D_TextBuf textBuf;
    PrintConsole console;
    char statusLine[128];
    int chunkIndex;
    int totalChunks;
    // The bottom screen shows the same art the game uses after boot (gfx/bottom.png), so the two halves of the console
    // do not disagree about what is being loaded.
    C3D_RenderTarget* bottomTarget;
    C3D_Tex artTexture;
    Tex3DS_SubTexture artSubtex;
    C2D_Image artImage;
    bool artReady;
    bool bottomPainted;      // the art is static: painting it once is enough, and twice costs a second scene a frame
    u64 drawMicros;          // total time spent drawing this screen, logged once the data is in
    int32_t drawCount;
} N3DSLoadingScreen;

// The port's palette, taken from the bottom screen's own drawing (see n3ds_livemap.c) plus the black it sits on.
#define N3DS_LOAD_BG C2D_Color32(0, 0, 0, 255)
#define N3DS_LOAD_FRAME C2D_Color32(0, 77, 0, 255)      // #004D00, the frame on the bottom art
#define N3DS_LOAD_TITLE C2D_Color32(123, 230, 99, 255)  // RGB 0x7be663, the light green of the energy plate
#define N3DS_LOAD_TEXT C2D_Color32(99, 230, 123, 255)   // RGB 0x63e67b
#define N3DS_LOAD_BAR C2D_Color32(41, 214, 0, 255)      // RGB 0x29d600, the mid green
#define N3DS_LOAD_DIM C2D_Color32(90, 90, 90, 255)      // the grey the menus use for inactive text
#define N3DS_LOAD_LOG C2D_Color32(74, 110, 74, 255)     // dim green, so the boot log stays background

static char gN3DSBootLogLines[N3DS_BOOT_LOG_MAX_LINES][N3DS_BOOT_LOG_LINE_CHARS];
static int gN3DSBootLogLineCount = 0;

#define N3DS_MONITOR_LINES 8

u64 gN3DSProfTicks[N3DS_PROF_COUNT];
u32 gN3DSProfFlushes;
#ifdef N3DS_FRAME_PROFILER
u64 gN3DSZoneTicks[N3DS_ZONE_COUNT];
u32 gN3DSZoneCalls[N3DS_ZONE_COUNT];
int gN3DSZoneStack[64];
int gN3DSZoneDepth;
u64 gN3DSZoneMark;

static const char* const gN3DSZoneNames[N3DS_ZONE_COUNT] = {
    "step (GML, collisions)", "frame start", "views (GML draw, layers)", "post (game -> screen)", "draw GUI (GML)",
    "overlays (map, monitor)", "room change", "after frame (gc, files)", "sprites", "sprite parts", "sprite pos",
    "tiles", "tiled", "text", "shapes", "surface draws", "surface targets", "view setup", "blend/alpha state",
    "GPU flushes", "texture loads", "frame wait (GPU, vblank)",
};

// The frame profiler's per-room report: each zone's exclusive time per frame, biggest first, then the GML profile.
static void N3DS_logRoomProfile(const char* roomName, uint32_t frames, double gpuMs, VMContext* vm) {
    if (frames == 0) return;
    int order[N3DS_ZONE_COUNT];
    for (int i = 0; i < N3DS_ZONE_COUNT; i++) order[i] = i;
    for (int i = 1; i < N3DS_ZONE_COUNT; i++) {
        for (int j = i; j > 0 && gN3DSZoneTicks[order[j]] > gN3DSZoneTicks[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    }
    logInfo("Profile: %s, %lu frames; GPU drawing %.2f ms/frame; CPU by zone (exclusive ms/frame, calls/frame):\n",
        roomName, (unsigned long) frames, gpuMs / (double) frames);
    for (int i = 0; i < N3DS_ZONE_COUNT; i++) {
        int z = order[i];
        double ms = N3DSProf_ms(gN3DSZoneTicks[z]) / (double) frames;
        // (Count-only zones, like tiles, show 0 ms.)
        if (ms < 0.05 && gN3DSZoneCalls[z] < frames * 100u) continue;
        logInfo("Profile:   %6.2f ms  %7.1f  %s\n", ms, (double) gN3DSZoneCalls[z] / (double) frames, gN3DSZoneNames[z]);
    }
    memset(gN3DSZoneTicks, 0, sizeof(gN3DSZoneTicks));
    memset(gN3DSZoneCalls, 0, sizeof(gN3DSZoneCalls));
#ifdef ENABLE_VM_GML_PROFILER
    char* report = Profiler_createReport(vm->profiler, 40, (int) frames);
    if (report != NULL) {
        // A line at a time: one log line has a size limit.
        logInfo("Profile: GML (self time incl. builtins, avg over the room's frames):\n");
        for (char* line = report; line != NULL && *line != '\0';) {
            char* next = strchr(line, '\n');
            if (next != NULL) *next++ = '\0';
            logInfo("Profile:   %s\n", line);
            line = next;
        }
        free(report);
    }
    Profiler_reset(vm->profiler);
#else
    (void) vm;
#endif
}
#endif

// Where one frame's time went, in ms. Step is the game logic (GML), wait is citro3d waiting for the GPU and VBlank,
// draw is building the GPU commands; IO, TX, FS and AU are slices of those (see n3ds_prof.h).
typedef enum {
    N3DS_FT_FRAME,
    N3DS_FT_STEP,
    N3DS_FT_WAIT,
    N3DS_FT_DRAW,
    N3DS_FT_IO,
    N3DS_FT_TEX,
    N3DS_FT_FS,
    N3DS_FT_AUDIO,
    N3DS_FT_PREWARM,
    N3DS_FT_COUNT
} N3DSFrameTime;

typedef struct {
    C2D_TextBuf textBuf;
    // Laid-out text, rebuilt only when a line changes (the values update a few times a second).
    char lines[N3DS_MONITOR_LINES][96];
    C2D_Text texts[N3DS_MONITOR_LINES];
    bool laidOut;
    double displayedFps;
    double displayed[N3DS_FT_COUNT];
    double displayedMaxFrameMs;
    double sampled[N3DS_FT_COUNT];
    double sampledMaxFrameMs;
    uint32_t sampledFrames;
    u64 sampleStartMs;
} N3DSDebugMonitor;

static bool fileExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void N3DS_formatDebugSize(char* out, size_t outSize, uint32_t bytes) {
    if (out == NULL || outSize == 0) return;

    double kib = (double) bytes / 1024.0;
    if (kib < 1024.0) {
        snprintf(out, outSize, "%.0f KB", kib);
        return;
    }

    snprintf(out, outSize, "%.2f MB", kib / 1024.0);
}

static void N3DSDebugMonitor_init(N3DSDebugMonitor* monitor) {
    if (monitor == NULL) return;
    memset(monitor, 0, sizeof(*monitor));
    monitor->textBuf = C2D_TextBufNew(1024);
    monitor->sampleStartMs = osGetTime();
}

static void N3DSDebugMonitor_free(N3DSDebugMonitor* monitor) {
    if (monitor == NULL) return;
    if (monitor->textBuf != NULL) {
        C2D_TextBufDelete(monitor->textBuf);
        monitor->textBuf = NULL;
    }
}

static void N3DSDebugMonitor_tickFrame(N3DSDebugMonitor* monitor, const double* frameTimes) {
    if (monitor == NULL) return;

    monitor->sampledFrames++;
    repeat(N3DS_FT_COUNT, i) monitor->sampled[i] += frameTimes[i];
    if (frameTimes[N3DS_FT_FRAME] > monitor->sampledMaxFrameMs) monitor->sampledMaxFrameMs = frameTimes[N3DS_FT_FRAME];
    u64 nowMs = osGetTime();
    u64 elapsedMs = nowMs - monitor->sampleStartMs;
    if (elapsedMs < 250) return;

    monitor->displayedFps = ((double) monitor->sampledFrames * 1000.0) / (double) elapsedMs;
    repeat(N3DS_FT_COUNT, i) {
        monitor->displayed[i] = monitor->sampled[i] / (double) monitor->sampledFrames;
        monitor->sampled[i] = 0.0;
    }
    monitor->displayedMaxFrameMs = monitor->sampledMaxFrameMs;
    monitor->sampledMaxFrameMs = 0.0;
    monitor->sampledFrames = 0;
    monitor->sampleStartMs = nowMs;
}

// Drawn on the top screen, the only screen the game uses.
static void N3DSDebugMonitor_draw(N3DSDebugMonitor* monitor, Runner* runner, Renderer* renderer) {
    if (monitor == NULL || runner == NULL || renderer == NULL || monitor->textBuf == NULL) return;

    const char* roomName = "(none)";
    if (runner->currentRoom != NULL && runner->currentRoom->name != NULL && runner->currentRoom->name[0] != '\0') {
        roomName = runner->currentRoom->name;
    }

    uint32_t atlasVRAMBytes = N3DSRenderer_getResidentAtlasVRAMBytes(renderer);
    uint32_t atlasPageCount = N3DSRenderer_getResidentAtlasPageCount(renderer);
    uint32_t directVRAMBytes = N3DSRenderer_getResidentDirectAssetVRAMBytes(renderer);
    // The heap takes the whole app region up front, so the region's free space says nothing: ask malloc.
    struct mallinfo mi = mallinfo();

    char vramUsed[24], vramTotal[24], heapUsed[24], heapTotal[24], linearFree[24];
    N3DS_formatDebugSize(vramUsed, sizeof(vramUsed), atlasVRAMBytes + directVRAMBytes);
    N3DS_formatDebugSize(vramTotal, sizeof(vramTotal), N3DS_TOTAL_VRAM_BYTES);
    N3DS_formatDebugSize(heapUsed, sizeof(heapUsed), (uint32_t) mi.uordblks);
    N3DS_formatDebugSize(heapTotal, sizeof(heapTotal), __ctru_heap_size);
    N3DS_formatDebugSize(linearFree, sizeof(linearFree), linearSpaceFree());

    const double* t = monitor->displayed;
    char fpsLine[96], timeLine[96], sliceLine[96], vramLine[96], ramLine[96], unimplLine[64], roomLine[96];
    snprintf(fpsLine, sizeof(fpsLine), "FPS %.1f  %.1fms  max %.0f", monitor->displayedFps, t[N3DS_FT_FRAME], monitor->displayedMaxFrameMs);
    snprintf(timeLine, sizeof(timeLine), "S %.1f  W %.1f  D %.1f  P %.1f", t[N3DS_FT_STEP], t[N3DS_FT_WAIT], t[N3DS_FT_DRAW], t[N3DS_FT_PREWARM]);
    snprintf(sliceLine, sizeof(sliceLine), "IO %.1f  TX %.1f  FS %.1f  AU %.1f", t[N3DS_FT_IO], t[N3DS_FT_TEX], t[N3DS_FT_FS], t[N3DS_FT_AUDIO]);
    snprintf(vramLine, sizeof(vramLine), "VRAM %s/%s  AT %lu", vramUsed, vramTotal, (unsigned long) atlasPageCount);
    snprintf(ramLine, sizeof(ramLine), "RAM %s/%s  L %s", heapUsed, heapTotal, linearFree);
    snprintf(unimplLine, sizeof(unimplLine), "UNIMPL %lu", (unsigned long) N3DS_unimplCount());
    snprintf(roomLine, sizeof(roomLine), "R %.28s", roomName);

    N3DSRenderer_beginScreenOverlay(renderer);
    const float boxX = 172.0f;
    const float boxY = 8.0f;
    const float boxW = 220.0f;
    const float boxH = 127.0f;
    const float textX = boxX + 7.0f;
    C2D_DrawRectSolid(boxX, boxY, N3DS_OVERLAY_DEPTH, boxW, boxH, C2D_Color32(8, 10, 16, 218));
    C2D_DrawRectSolid(boxX, boxY, N3DS_OVERLAY_DEPTH, boxW, 2.0f, C2D_Color32(77, 118, 255, 245));
    C2D_DrawRectSolid(boxX, boxY + boxH - 1.0f, N3DS_OVERLAY_DEPTH, boxW, 1.0f, C2D_Color32(32, 46, 78, 230));
    // System-font text, one quad per character (the old per-pixel bitmap font cost ~2000 quads a frame, half of
    // citro2d's per-frame object budget).
    const char* lines[N3DS_MONITOR_LINES] = { "DBG", fpsLine, timeLine, sliceLine, vramLine, ramLine, unimplLine, roomLine };
    const float xs[N3DS_MONITOR_LINES] = { textX, boxX + 42.0f, textX, textX, textX, textX, textX, textX };
    const float ys[N3DS_MONITOR_LINES] = { 5.0f, 5.0f, 22.0f, 39.0f, 56.0f, 73.0f, 90.0f, 107.0f };
    const u32 colors[N3DS_MONITOR_LINES] = {
        C2D_Color32(255, 255, 255, 255), C2D_Color32(196, 230, 255, 255), C2D_Color32(196, 230, 255, 255),
        C2D_Color32(146, 188, 236, 255), C2D_Color32(196, 230, 255, 255), C2D_Color32(196, 230, 255, 255),
        C2D_Color32(255, 196, 196, 255), C2D_Color32(255, 230, 163, 255),
    };
    bool changed = !monitor->laidOut;
    repeat(N3DS_MONITOR_LINES, i) {
        if (strcmp(monitor->lines[i], lines[i]) != 0) changed = true;
    }
    if (changed) {
        C2D_TextBufClear(monitor->textBuf);
        repeat(N3DS_MONITOR_LINES, i) {
            snprintf(monitor->lines[i], sizeof(monitor->lines[i]), "%s", lines[i]);
            C2D_TextParse(&monitor->texts[i], monitor->textBuf, monitor->lines[i]);
            C2D_TextOptimize(&monitor->texts[i]);
        }
        monitor->laidOut = true;
    }
    repeat(N3DS_MONITOR_LINES, i) {
        C2D_DrawText(&monitor->texts[i], C2D_WithColor, xs[i], boxY + ys[i], N3DS_OVERLAY_DEPTH, 0.42f, 0.42f, colors[i]);
    }
    N3DSRenderer_endScreenOverlay(renderer);
}

// The FPS counter (Display options): the debug monitor's frame rate, small, in the top screen's bottom-left corner.
typedef struct {
    C2D_TextBuf buf;
    C2D_Text text;
    int shown;
} N3DSFpsCounter;

static void N3DSFpsCounter_draw(N3DSFpsCounter* counter, Renderer* renderer, double fps) {
    int value = (int) (fps + 0.5);
    if (counter->buf == NULL) {
        counter->buf = C2D_TextBufNew(16);
        counter->shown = -1;
    }
    if (value != counter->shown) {
        char line[16];
        snprintf(line, sizeof(line), "%d", value);
        C2D_TextBufClear(counter->buf);
        C2D_TextParse(&counter->text, counter->buf, line);
        C2D_TextOptimize(&counter->text);
        counter->shown = value;
    }
    const float scale = 0.4f;
    float w = 0.0f, h = 0.0f;
    C2D_TextGetDimensions(&counter->text, scale, scale, &w, &h);
    float x = 4.0f, y = (float) N3DS_TOP_SCREEN_H - h - 3.0f;
    N3DSRenderer_beginScreenOverlay(renderer);
    C2D_DrawText(&counter->text, C2D_WithColor, x + 1.0f, y + 1.0f, N3DS_OVERLAY_DEPTH, scale, scale, C2D_Color32(0, 0, 0, 150));
    C2D_DrawText(&counter->text, C2D_WithColor, x, y, N3DS_OVERLAY_DEPTH, scale, scale, C2D_Color32(255, 255, 255, 190));
    N3DSRenderer_endScreenOverlay(renderer);
}

// The VM calls a builtin over a game script of the same name, for GameMaker 2.3+ games that carry compatibility
// scripts for newer builtins. A game made before 2.3 can't be doing that: its script is the real thing (AM2R's
// string_split(str, sep, index) returns one part; the GMS2 builtin returns an array, and every button hint in the
// pause screen read "<array...>").
static void N3DS_preferGameScripts(VMContext* vm, DataWin* dataWin) {
    if (DataWin_isVersionAtLeast(dataWin, 2, 3, 0, 0)) return;
    for (uint32_t i = 0; i < vm->funcCallCacheCount; i++) {
        if (vm->funcCallCache[i].builtin == NULL) continue;
        const char* name = dataWin->func.functions[i].name;
        ptrdiff_t script = shgeti(vm->codeIndexByName, (char*) name);
        if (script < 0) continue;
        vm->funcCallCache[i].builtin = NULL;
        vm->funcCallCache[i].scriptCodeIndex = vm->codeIndexByName[script].value;
        logInfo("VM: calls to %s go to the game's own script\n", name);
    }
}

static char* chooseDataWinPath(void) {
    if (fileExists("romfs:/data.win")) return safeStrdup("romfs:/data.win");
    return safeStrdup(N3DS_SD_DIR "data.win");
}

static void N3DSLoadingScreen_free(N3DSLoadingScreen* screen) {
    if (screen == NULL) return;
    if (screen->textBuf != NULL) {
        C2D_TextBufDelete(screen->textBuf);
        screen->textBuf = NULL;
    }
    if (screen->target != NULL) {
        C3D_RenderTargetDelete(screen->target);
        screen->target = NULL;
    }
    if (screen->artReady) {
        C3D_TexDelete(&screen->artTexture);
        screen->artReady = false;
    }
    // Same rule as the top target: a screen target has to go before the renderer makes its own for that screen.
    if (screen->bottomTarget != NULL) {
        C3D_RenderTargetDelete(screen->bottomTarget);
        screen->bottomTarget = NULL;
    }
}

// 8x8 Morton order, the layout citro3d's textures use (same helper the renderer uploads with).
static inline uint32_t N3DSLoadingMorton(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
}

// Loads gfx/bottom.png (SD first, then the copy in romfs) into a texture of its own, so the boot screen can show the
// bottom art before the renderer exists. Failure is silent: the bottom screen then stays black, as it used to.
static void N3DSLoadingScreen_loadArt(N3DSLoadingScreen* screen) {
    if (screen == NULL || !screen->useCitro2D) return;

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load(N3DS_SD_DIR "gfx/bottom.png", &width, &height, &channels, 4);
    if (pixels == NULL) pixels = stbi_load("romfs:/gfx/bottom.png", &width, &height, &channels, 4);
    if (pixels == NULL || width <= 0 || height <= 0 || width > 1024 || height > 1024) {
        if (pixels != NULL) stbi_image_free(pixels);
        return;
    }

    uint32_t texW = 8u, texH = 8u;
    while (texW < (uint32_t) width) texW <<= 1;
    while (texH < (uint32_t) height) texH <<= 1;
    if (!C3D_TexInit(&screen->artTexture, (u16) texW, (u16) texH, GPU_RGBA8)) {
        stbi_image_free(pixels);
        return;
    }
    C3D_TexSetFilter(&screen->artTexture, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&screen->artTexture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    screen->artTexture.border = 0x00000000u;

    uint32_t* destination = (uint32_t*) screen->artTexture.data;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const stbi_uc* pixel = pixels + ((size_t) y * (size_t) width + (size_t) x) * 4u;
            uint32_t rgba = ((uint32_t) pixel[0] << 24) | ((uint32_t) pixel[1] << 16) |
                ((uint32_t) pixel[2] << 8) | (uint32_t) pixel[3];
            destination[((size_t) ((uint32_t) y >> 3) * (size_t) (texW >> 3) + (size_t) ((uint32_t) x >> 3)) * 64u +
                (size_t) N3DSLoadingMorton((uint32_t) x & 7u, (uint32_t) y & 7u)] = rgba;
        }
    }
    GSPGPU_FlushDataCache(screen->artTexture.data, screen->artTexture.size);
    stbi_image_free(pixels);

    screen->artSubtex.width = (u16) width;
    screen->artSubtex.height = (u16) height;
    screen->artSubtex.left = 0.0f;
    screen->artSubtex.top = 1.0f;
    screen->artSubtex.right = (float) width / (float) texW;
    screen->artSubtex.bottom = 1.0f - (float) height / (float) texH;
    screen->artImage.tex = &screen->artTexture;
    screen->artImage.subtex = &screen->artSubtex;
    screen->artReady = true;
}

static void N3DS_appendBootLog(const char* message) {
    if (message == NULL || message[0] == '\0') return;

    if (gN3DSBootLogLineCount < N3DS_BOOT_LOG_MAX_LINES) {
        snprintf(
            gN3DSBootLogLines[gN3DSBootLogLineCount],
            sizeof(gN3DSBootLogLines[gN3DSBootLogLineCount]),
            "%s",
            message
        );
        gN3DSBootLogLineCount++;
        return;
    }

    repeat(N3DS_BOOT_LOG_MAX_LINES - 1, i) {
        snprintf(
            gN3DSBootLogLines[i],
            sizeof(gN3DSBootLogLines[i]),
            "%s",
            gN3DSBootLogLines[i + 1]
        );
    }
    snprintf(
        gN3DSBootLogLines[N3DS_BOOT_LOG_MAX_LINES - 1],
        sizeof(gN3DSBootLogLines[N3DS_BOOT_LOG_MAX_LINES - 1]),
        "%s",
        message
    );
}

static void N3DSLoadingScreen_draw(N3DSLoadingScreen* screen) {
    if (screen == NULL) return;

    float progress = 0.0f;
    if (screen->totalChunks > 0) {
        progress = (float) screen->chunkIndex / (float) screen->totalChunks;
        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }

    if (screen->useCitro2D && screen->target != NULL && screen->textBuf != NULL) {
        u64 drawStart = svcGetSystemTick();
        const float barX = 28.0f;
        const float barY = 146.0f;
        const float barW = 344.0f;
        const float barH = 14.0f;
        const float fillW = (barW - 4.0f) * progress;

        C2D_Text title;
        C2D_Text status;
        C2D_Text detail;
        char detailLine[64];
        snprintf(detailLine, sizeof(detailLine), "%d / %d", screen->chunkIndex, screen->totalChunks);

        N3DSRenderer_frameGate();
        C2D_TargetClear(screen->target, N3DS_LOAD_BG);
        C2D_SceneBegin(screen->target);

        // A one-pixel frame six pixels in, the same shape as the bottom screen's art, so the two screens read as one
        // console rather than as a port with its own idea of style.
        const float frameX = 6.0f;
        const float frameY = 6.0f;
        const float frameW = 400.0f - 12.0f;
        const float frameH = 240.0f - 12.0f;
        C2D_DrawRectSolid(frameX, frameY, 0.5f, frameW, 1.0f, N3DS_LOAD_FRAME);
        C2D_DrawRectSolid(frameX, frameY + frameH - 1.0f, 0.5f, frameW, 1.0f, N3DS_LOAD_FRAME);
        C2D_DrawRectSolid(frameX, frameY, 0.5f, 1.0f, frameH, N3DS_LOAD_FRAME);
        C2D_DrawRectSolid(frameX + frameW - 1.0f, frameY, 0.5f, 1.0f, frameH, N3DS_LOAD_FRAME);

        // The bar itself: dark green frame, black track, mid green fill - the colours of the bottom screen's energy
        // plate, which is the one place the game shows a bar of its own.
        C2D_DrawRectSolid(barX, barY, 0.5f, barW, barH, N3DS_LOAD_FRAME);
        C2D_DrawRectSolid(barX + 1.0f, barY + 1.0f, 0.5f, barW - 2.0f, barH - 2.0f, N3DS_LOAD_BG);
        if (fillW > 0.0f) {
            C2D_DrawRectSolid(barX + 2.0f, barY + 2.0f, 0.5f, fillW, barH - 4.0f, N3DS_LOAD_BAR);
            C2D_DrawRectSolid(barX + 2.0f, barY + 2.0f, 0.5f, fillW, 1.0f, N3DS_LOAD_TITLE);
        }

        C2D_TextBufClear(screen->textBuf);
        C2D_TextParse(&title, screen->textBuf, "Loading Game Data");
        C2D_TextParse(&status, screen->textBuf, screen->statusLine);
        C2D_TextParse(&detail, screen->textBuf, detailLine);
        C2D_TextOptimize(&title);
        C2D_TextOptimize(&status);
        C2D_TextOptimize(&detail);

        C2D_DrawText(&title, C2D_WithColor, 28.0f, 62.0f, 0.5f, 0.70f, 0.70f, N3DS_LOAD_TITLE);
        C2D_DrawText(&status, C2D_WithColor, 28.0f, 100.0f, 0.5f, N3DS_LOADING_TEXT_SCALE, N3DS_LOADING_TEXT_SCALE, N3DS_LOAD_TEXT);
        C2D_DrawText(&detail, C2D_WithColor, 28.0f, 166.0f, 0.5f, 0.32f, 0.32f, N3DS_LOAD_DIM);

        int firstLogLine = gN3DSBootLogLineCount > 3 ? gN3DSBootLogLineCount - 3 : 0;
        float logY = 188.0f;
        for (int i = firstLogLine; i < gN3DSBootLogLineCount; ++i) {
            C2D_Text logLine;
            C2D_TextParse(&logLine, screen->textBuf, gN3DSBootLogLines[i]);
            C2D_TextOptimize(&logLine);
            C2D_DrawText(&logLine, C2D_WithColor, 28.0f, logY, 0.5f, 0.25f, 0.25f, N3DS_LOAD_LOG);
            logY += 12.0f;
        }

        // The bottom screen: the same art the game shows once it is running, so the console does not go black for the
        // nine seconds the data takes to load.
        if (screen->bottomTarget != NULL && !screen->bottomPainted) {
            screen->bottomPainted = true;
            C2D_TargetClear(screen->bottomTarget, N3DS_LOAD_BG);
            C2D_SceneBegin(screen->bottomTarget);
            if (screen->artReady) {
                C2D_DrawImageAt(screen->artImage, 0.0f, 0.0f, 0.5f, NULL,
                    320.0f / (float) screen->artSubtex.width, 240.0f / (float) screen->artSubtex.height);
            }
        }

        N3DSRenderer_frameEnd();
        gspWaitForVBlank();
        screen->drawMicros += (svcGetSystemTick() - drawStart) * 1000000ull / SYSCLOCK_ARM11;
        screen->drawCount++;
        return;
    }

    int filled = (int) lroundf(progress * 24.0f);
    if (filled < 0) filled = 0;
    if (filled > 24) filled = 24;

    char bar[25];
    repeat(24, i) {
        bar[i] = (int) i < filled ? '#' : '-';
    }
    bar[24] = '\0';

    consoleSelect(&screen->console);
    printf("\x1b[2J");
    printf("\x1b[4;8HLoading Game Data");
    printf("\x1b[7;8H%s", screen->statusLine);
    printf("\x1b[10;8H[%s]", bar);
    printf("\x1b[12;8H%d / %d chunks", screen->chunkIndex, screen->totalChunks);
    printf("\x1b[15;8HPlease wait...");
    int firstLogLine = gN3DSBootLogLineCount > 5 ? gN3DSBootLogLineCount - 5 : 0;
    for (int i = firstLogLine; i < gN3DSBootLogLineCount; ++i) {
        printf("\x1b[%d;4H%s", 17 + (i - firstLogLine), gN3DSBootLogLines[i]);
    }

    gfxFlushBuffers();
    gfxSwapBuffers();
    gspWaitForVBlank();
}

static void N3DSLoadingScreen_set(N3DSLoadingScreen* screen, const char* statusLine, int chunkIndex, int totalChunks) {
    if (screen == NULL) return;
    snprintf(screen->statusLine, sizeof(screen->statusLine), "%s", statusLine != NULL ? statusLine : "");
    screen->chunkIndex = chunkIndex;
    screen->totalChunks = totalChunks;
    N3DSLoadingScreen_draw(screen);
}

static void N3DS_waitForStartExitScreen(N3DSLoadingScreen* screen, const char* statusLine) {
    N3DSLoadingScreen_set(screen, statusLine, 1, 1);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
}

static void N3DSDataWinProgressCallback(const char* chunkName, int chunkIndex, int totalChunks, MAYBE_UNUSED DataWin* dataWin, void* userData) {
    N3DSLoadingScreen* screen = (N3DSLoadingScreen*) userData;
    if (screen == NULL) return;

    // A repaint costs a frame plus the bottom screen's first paint, while some chunks parse in a millisecond: without
    // this the progress display can cost more than the parsing it is reporting. The final state is always drawn by
    // the explicit N3DSLoadingScreen_set calls that follow.
    static u64 lastRepaintMs;
    u64 nowMs = osGetTime();
    if (lastRepaintMs != 0 && nowMs - lastRepaintMs < 80) return;
    lastRepaintMs = nowMs;

    char status[128];
    snprintf(status, sizeof(status), "Parsing chunk %.4s", chunkName != NULL ? chunkName : "----");
    N3DSLoadingScreen_set(screen, status, chunkIndex + 1, totalChunks);
}

static void N3DS_noopSetWindowTitle(MAYBE_UNUSED const char* title) {}
static void N3DS_noopSetWindowSize(MAYBE_UNUSED int32_t width, MAYBE_UNUSED int32_t height) {}
#ifndef N3DS_DEFAULT_SCREEN_MODE
#define N3DS_DEFAULT_SCREEN_MODE N3DS_SCREEN_1X
#endif
static N3DSScreenMode gScreenMode = N3DS_DEFAULT_SCREEN_MODE;
static bool gFrameskip = false;
static int32_t gWindowW = N3DS_TOP_SCREEN_W;
static int32_t gWindowH = N3DS_TOP_SCREEN_H;

static const char* N3DS_screenModeName(N3DSScreenMode mode) {
    switch (mode) {
        case N3DS_SCREEN_WIDE: return "wide (1:1, fills the screen)";
        case N3DS_SCREEN_STRETCH: return "stretch (fills the screen, squashed)";
        case N3DS_SCREEN_1X: return "point-for-point (320x240, 1:1, side bars)";
        default: return "pillarbox (1:1)";
    }
}

bool N3DS_getFrameskip(void) {
    return gFrameskip;
}

static bool gFpsCounter = false;

bool N3DS_getFpsCounter(void) {
    return gFpsCounter;
}

void N3DS_setFpsCounter(bool on) {
    gFpsCounter = on;
}

void N3DS_setFrameskip(bool on) {
    if (on == gFrameskip) return;
    gFrameskip = on;
    logInfo("Frameskip: %s\n", on ? "on (30 fps, 2 steps a frame)" : "off");
}

N3DSScreenMode N3DS_getScreenMode(void) {
    return gScreenMode;
}

void N3DS_setScreenMode(N3DSScreenMode mode) {
    if ((unsigned) mode >= (unsigned) N3DS_SCREEN_MODE_COUNT || mode == gScreenMode) return;
    gScreenMode = mode;
    logInfo("Screen mode: %s\n", N3DS_screenModeName(gScreenMode));
}

static bool N3DS_getWindowSize(int32_t* outW, int32_t* outH) {
    *outW = gWindowW;
    *outH = gWindowH;
    return true;
}

static void N3DS_logMemory(const char* when) {
    // osGetMemRegionFree(APPLICATION) is 0 from the moment libctru takes the region, so it says nothing; the arena and
    // its free blocks come from mallinfo instead, which is what a failing malloc is really measuring.
    struct mallinfo mi = mallinfo();
    logInfo("Memory (%s): app free %lu KB of %lu KB, heap used %lu KB, heap free %lu KB, heap arena %lu KB, "
        "linear free %lu KB, vram free %lu KB\n", when,
        (unsigned long) (osGetMemRegionFree(MEMREGION_APPLICATION) / 1024u),
        (unsigned long) (osGetMemRegionSize(MEMREGION_APPLICATION) / 1024u),
        (unsigned long) (mi.uordblks / 1024u), (unsigned long) (mi.fordblks / 1024u),
        (unsigned long) (mi.arena / 1024u),
        (unsigned long) (linearSpaceFree() / 1024u),
        (unsigned long) (vramSpaceFree() / 1024u));
}

// 3DS threads start with the VFP in "RunFast" with rounding toward zero (FPSCR 0x03C00000); a PC rounds to nearest
// and keeps denormals. GameMaker games' movement and collision code leans on exact double arithmetic (fractional
// speeds, floor() of positions, pixel-snapping loops), so round toward zero puts Samus a hair off where the game
// expects her (47.9999 instead of 48) and she can slip into or through blocks while moving vertically. The game
// runs on this thread only. Returns the FPSCR found, for the log.
static uint32_t N3DS_useIeeeRounding(void) {
    uint32_t fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    uint32_t ieee = fpscr & ~((3u << 22) | (1u << 24)); // round to nearest, no flush-to-zero
    __asm__ volatile("vmsr fpscr, %0" : : "r"(ieee));
    return fpscr;
}

// Sleep mode (closing the lid) and the HOME Menu: aptMainLoop, called once a frame, handles both and blocks until the
// console wakes / the game is back, with the worker threads idle on their events; NDSP stops and restarts its own
// output. On the way back the frame clock starts over, so the first frame's delta_time isn't the time asleep.
static volatile bool gN3DSResumed = false;
static aptHookCookie gN3DSAptCookie;

static void N3DS_aptHook(APT_HookType hook, MAYBE_UNUSED void* param) {
    if (hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONRESTORE) gN3DSResumed = true;
}

int main(int argc, char** argv) {
    gfxInitDefault();
    romfsInit();
    osSetSpeedupEnable(true);
    APT_SetAppCpuTimeLimit(30);
    aptSetSleepAllowed(true);
    aptHook(&gN3DSAptCookie, N3DS_aptHook, NULL);

    mkdir("sdmc:/3ds", 0777);
    mkdir(N3DS_SD_DIR, 0777);
    N3DSLog_init();
    logInfo("Butterscotch 3DS (%s %s)\n", BUTTERSCOTCH_COMMIT_HASH, BUTTERSCOTCH_COMMIT_DATE);
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);
    logInfo("Console: %s, speedup %s\n", isNew3DS ? "New 3DS" : "Old 3DS", isNew3DS ? "on" : "n/a");
    // Landmark for crash dumps: dumps only carry SP, so each thread says where its stack lives.
    logInfo("N3DS: main thread stack near 0x%08x\n", (unsigned) (uintptr_t) &argc);
    uint32_t startFpscr = N3DS_useIeeeRounding();
    {
        uint32_t fpscr;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
        volatile double third = 1.0 / 3.0, tenth = 0.1;
        logInfo("FPU: FPSCR was %08lx, now %08lx (round to nearest); 1/3*3 %s 1, 0.1+0.2 %s 0.3 as on a PC\n",
            (unsigned long) startFpscr, (unsigned long) fpscr, third * 3.0 == 1.0 ? "==" : "!=",
            tenth + 0.2 == 0.3 ? "==" : "!=");
    }
    N3DS_logMemory("startup");

    bool citroReady = false;
    if (C3D_Init(0x80000) && C2D_Init(N3DS_C2D_MAX_OBJECTS)) {
        C2D_Prepare();
        citroReady = true;
    } else {
        C2D_Fini();
        C3D_Fini();
    }

    char* dataWinPath = chooseDataWinPath();
    logInfo("Loading %s\n", dataWinPath);

    N3DSLoadingScreen loadingScreen = {0};
    if (citroReady) {
        loadingScreen.useCitro2D = true;
        loadingScreen.target = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
        loadingScreen.textBuf = C2D_TextBufNew(512);
        if (loadingScreen.target == NULL || loadingScreen.textBuf == NULL) {
            N3DSLoadingScreen_free(&loadingScreen);
            loadingScreen.useCitro2D = false;
        } else {
            // The bottom screen gets the same art the game uses, and its own target: the renderer only exists from
            // about nine seconds in, and until then nothing else would draw there.
            loadingScreen.bottomTarget = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
            N3DSLoadingScreen_loadArt(&loadingScreen);
        }
    } else {
        consoleInit(GFX_TOP, &loadingScreen.console);
    }

    // The game's files on the card are installed once (tools/n3ds/make_sd.sh); a build that needs newer ones says so.
    {
        int cardRev = -1;
        FILE* revFile = fopen(N3DS_SD_DIR "sd_data_rev.txt", "r");
        if (revFile != NULL) {
            if (fscanf(revFile, "%d", &cardRev) != 1) cardRev = -1;
            fclose(revFile);
        }
        if (cardRev != N3DS_SD_DATA_REV || !fileExists(dataWinPath)) {
            char message[128];
            if (!fileExists(dataWinPath)) snprintf(message, sizeof(message), "No game files in %s. Press START.", N3DS_SD_DIR);
            else snprintf(message, sizeof(message), "Game files on the SD card are rev %d, this build needs rev %d. Press START.", cardRev, N3DS_SD_DATA_REV);
            logError("%s\n", message);
            N3DS_waitForStartExitScreen(&loadingScreen, message);
            N3DSLoadingScreen_free(&loadingScreen);
            N3DSLog_close();
            return 1;
        }
    }

    N3DSLoadingScreen_set(&loadingScreen, "Scanning data.win", 0, 1);

    DataWinParserOptions options = {0};
    options.parseGen8 = true;
    options.parseOptn = true;
    options.parseLang = true;
    options.parseExtn = true;
    options.parseSond = true;
    options.parseAgrp = true;
    options.parseSprt = true;
    options.parseBgnd = true;
    options.parsePath = true;
    options.parseScpt = true;
    options.parseGlob = true;
    options.parseShdr = true;
    options.parseFont = true;
    options.parseTmln = true;
    options.parseObjt = true;
    options.parseRoom = true;
    options.parseTpag = true;
    options.parseCode = true;
    options.parseVari = true;
    options.parseFunc = true;
    options.parseStrg = true;
    // Textures come from n3ds-preprocess (gfx/), audio is stubbed: TXTR and AUDO stay on the card.
    options.parseTxtr = false;
    options.parseAudo = false;
    options.skipLoadingPreciseMasksForNonPreciseSprites = true;
    options.loadType = DATAWINLOADTYPE_LOAD_PER_CHUNK;
    options.progressCallback = N3DSDataWinProgressCallback;
    options.progressCallbackUserData = &loadingScreen;
    DataWin* dataWin = DataWin_parse(dataWinPath, options);

    if (dataWin == NULL) {
        logError("Failed to load %s\n", dataWinPath);
        N3DS_waitForStartExitScreen(&loadingScreen, "Failed to load data.win. Press START.");
        N3DSLoadingScreen_free(&loadingScreen);
        N3DSLog_close();
        return 1;
    }
    Gen8* gen8 = &dataWin->gen8;
    logInfo("Loaded \"%s\" [WAD %u / GM %u.%u.%u.%u], %dx%d\n", gen8->name, gen8->wadVersion,
        dataWin->detectedFormat.major, dataWin->detectedFormat.minor, dataWin->detectedFormat.release, dataWin->detectedFormat.build,
        (int) gen8->defaultWindowWidth, (int) gen8->defaultWindowHeight);
    N3DS_logMemory("data.win parsed");
    logInfo("Loading screen: %d draws, %.0f ms total\n", (int) loadingScreen.drawCount,
        (double) loadingScreen.drawMicros / 1000.0);

    // Game files next to data.win (romfs or the SD folder), saves in the SD folder: upstream's overlay file system.
    char* dataWinDir = safeStrdup(dataWinPath);
    bsGetDirname(dataWinDir);
    FileSystem* fileSystem = N3DSCachedFileSystem_create((FileSystem*) OverlayFileSystem_create(dataWinDir, N3DS_SD_DIR));
    logInfo("Files: %s (saves %s)\n", dataWinDir, N3DS_SD_DIR);
    free(dataWinDir);
    free(dataWinPath);
#ifdef N3DS_ENABLE_AUDIO
    AudioSystem* audioSystem = (AudioSystem*) N3DSAudioSystem_create(); // initialised by Runner_create
#else
    AudioSystem* audioSystem = (AudioSystem*) NoopAudioSystem_create();
#endif

    N3DSLoadingScreen_set(&loadingScreen, "Initializing renderer", 2, 2);
    // Deleting a screen target unlinks that screen: the loading screen's target must go before the renderer makes its own.
    N3DSLoadingScreen_free(&loadingScreen);
    VMContext* vm = VM_create(dataWin);
    N3DS_preferGameScripts(vm, dataWin);
    Renderer* renderer = N3DSRenderer_create();
    // Input playback runs use a fixed seed, like the desktop runner with --seed 1, so both sides stay in step.
    bool deterministic = fileExists(N3DS_SD_DIR "inputs.json");
    if (deterministic) vm->hasFixedSeed = true;
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem, deterministic ? 1u : (uint32_t) osGetTime());

    if (!N3DSRenderer_isReady(renderer)) {
        const char* error = N3DSRenderer_getStartupError(renderer);
        logError("Renderer: %s\n", error != NULL ? error : "init failed");
        // This build only uses the top screen, so a failure to start has to be readable there.
        consoleInit(GFX_TOP, NULL);
        printf("%s\nPress START to exit.\n", error != NULL ? error : "Renderer init failed");
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            gspWaitForVBlank();
        }
        return 1;
    }

    N3DSDebugMonitor debugMonitor;
    N3DSDebugMonitor_init(&debugMonitor);

    // The PC version's code paths (input, saves, menus) are the ones AM2R-era games expect.
    runner->osType = OS_WINDOWS;
    runner->setWindowTitle = N3DS_noopSetWindowTitle;
    runner->setWindowSize = N3DS_noopSetWindowSize;
    runner->getWindowSize = N3DS_getWindowSize;
    char** gameArgs = NULL;
    arrput(gameArgs, safeStrdup(argc > 0 && argv[0] != NULL ? argv[0] : "butterscotch"));
    Runner_setGameArgs(runner, gameArgs, (int32_t) arrlen(gameArgs));
    N3DSInput_init(runner);
    N3DSPause_init(runner, renderer);
    N3DSLiveMap_init(runner, renderer);
    N3DSAm2r_init(runner); // loads the font cache itself, once the port's characters are in the glyph set
    // The bottom screen's artwork, if the player put one on the SD card. Optional: without the file (or with one
    // that will not decode) the screen keeps the plain black it always had.
    {
        int bottomW = 0, bottomH = 0, bottomChannels = 0;
        // The SD card copy wins, so the art can be replaced without rebuilding; otherwise the one packaged into
        // the title is used, and a build without either keeps the plain black.
        const char* bottomPath = N3DS_SD_DIR "gfx/bottom.png";
        stbi_uc* bottomPixels = stbi_load(bottomPath, &bottomW, &bottomH, &bottomChannels, 4);
        if (bottomPixels == NULL) {
            bottomPath = "romfs:/gfx/bottom.png";
            bottomPixels = stbi_load(bottomPath, &bottomW, &bottomH, &bottomChannels, 4);
        }
        if (bottomPixels != NULL) {
            if (!N3DSRenderer_setBottomBackground(renderer, bottomPixels, (uint32_t) bottomW, (uint32_t) bottomH)) {
                logWarn("N3DS: %s (%dx%d) could not be uploaded; the bottom screen stays black\n", bottomPath,
                    bottomW, bottomH);
            } else {
                logInfo("N3DS: bottom screen art %dx%d from %s\n", bottomW, bottomH, bottomPath);
            }
            stbi_image_free(bottomPixels);
        }
    }
    logInfo("Screen mode: %s\n", N3DS_screenModeName(gScreenMode));
    N3DSFpsCounter fpsCounter = {0};
#ifdef ENABLE_VM_GML_PROFILER
    Profiler_setEnabled(&vm->profiler, true);
#endif
    // Keyboard playback in the desktop --playback-inputs format (e.g. a converted TAS), for regression runs.
    InputRecording* inputPlayback = NULL;
    int32_t inputFrame = 0;
    if (fileExists(N3DS_SD_DIR "inputs.json")) inputPlayback = InputRecording_createPlayer(N3DS_SD_DIR "inputs.json", NULL);

    // Boot diagnostics: with sdmc:/3ds/am2r/bootshots.txt on the card, the first frames after the loading screen are
    // saved as shots/boot_<n>.png. The gate has just waited for the previous frame, so each file is what was presented.
    int32_t bootShotsLeft = fileExists(N3DS_SD_DIR "bootshots.txt") ? 12 : 0;
    int32_t bootShotIndex = 0;
    int32_t bootPendingShot = -1;
    // The game's first room and the transition into the title draw a collage of unrelated menu artwork (screenshots of
    // the boot frames show the title's UI pieces tiled across the screen): whatever the game draws there, it is not
    // what the player should see. Keep both screens black until a real room is up; the title then fades in from black,
    // which is how it starts anyway.
    bool bootBlackout = true;
    if (fileExists(N3DS_SD_DIR "drawtrace.txt")) {
        N3DSRenderer_startDrawTrace(renderer, 6);
        bootBlackout = false; // trace what the game actually draws, not the blackout
        logInfo("N3DS: draw trace on (6 frames)\n");
    }

    N3DSRenderer_frameGate();
    Runner_initFirstRoom(runner);
    // The first frame is drawn straight after this, and the loading screen has already gone: without warming the room
    // here that frame reads its artwork from the card while drawing (the log showed io 33 ms in it), so it showed the
    // textures' previous contents. Every later room is warmed at the end of its transition frame; the first one gets
    // the same treatment, while the blackened startup frame below is all that is on screen.
    {
        u64 prewarmStart = N3DSProf_begin();
        N3DSRenderer_prewarmRoom(renderer, runner);
        N3DSProf_end(N3DS_PROF_PREWARM, prewarmStart);
    }
    // The game has now run oControl's font_replace calls, so every strip is in the atlas: keep them for the next boot.
    N3DSFontReplace_saveCache(runner);
    renderer->vtable->flush(renderer);
    // Nothing has been drawn for either screen yet in this frame, so clear them: an opened frame presents whatever the
    // buffers hold, which was leftover from before the loading screen's target was released.
    N3DSRenderer_clearScreens(renderer);
    if (bootShotsLeft > 0) {
        bootShotsLeft--;
        bootPendingShot = bootShotIndex++;
    }
    N3DSRenderer_frameEnd();
    N3DSRenderer_collectGarbage(renderer, false);
    N3DS_logMemory("first room");

    bool debugMonitorVisible = false; // Start+Select shows it
    const Room* lastRoom = NULL;
    uint64_t lastFrameStartTime = nowNanos();
    u64 statsStartMs = osGetTime();
    uint32_t statsFrames = 0;
    uint32_t gStackLowest = 0;
    double statsStepMs = 0.0, statsDrawMs = 0.0, statsWaitMs = 0.0;
    uint32_t statsSprites = 0, statsParts = 0, statsFrags = 0, statsGlyphs = 0, statsFlushes = 0;
    double pacedSpeed = 0.0;
    uint32_t lastUnimplCount = 0;
    u64 lastSlowLogMs = 0;
    struct {
        uint32_t frames;
        double stepMs, waitMs, drawMs, frameMs, worstMs, flushes, instances, gpuMs;
    } roomStats = {0};
    double statsMaxFrameMs = 0.0;

    while (aptMainLoop() && !runner->shouldExit) {
        // citro3d paces frames on VBlank at the room speed; a second sleep-based pacer here halved the rate whenever
        // the two drifted out of phase.
        double gameSpeed = Runner_getEffectiveGameSpeed(runner);
        if (gameSpeed <= 0.0 || gameSpeed > 60.0) gameSpeed = 60.0;
        // Frameskip: two game steps per drawn frame, shown at half the room speed (30 fps for 60): the game keeps its
        // speed and drawing costs half as much.
        int32_t stepsPerFrame = gFrameskip ? 2 : 1;
        double frameRate = gameSpeed / (double) stepsPerFrame;
        if (frameRate != pacedSpeed) {
            C3D_FrameRate((float) frameRate);
            pacedSpeed = frameRate;
        }

        u64 frameStartTick = svcGetSystemTick();
        memset(gN3DSProfTicks, 0, sizeof(gN3DSProfTicks));
        gN3DSProfFlushes = 0;
        uint64_t frameStartNow = nowNanos();
        if (gN3DSResumed) {
            gN3DSResumed = false;
            lastFrameStartTime = frameStartNow - (uint64_t) (1.0e9 / frameRate);
        }
        // delta_time is per step.
        runner->deltaTime = (int64_t) (frameStartNow - lastFrameStartTime) / 1000.0 / (double) stepsPerFrame;
        lastFrameStartTime = frameStartNow;

        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerGamepad_beginFrame(runner->gamepads);
        RunnerMouse_beginFrame(runner->mouse);
        N3DSInput_update(runner);
        InputRecording_processFrame(inputPlayback, runner->keyboard, inputFrame++);
        if (N3DSInput_takeMonitorToggle()) debugMonitorVisible = !debugMonitorVisible;

        // The GPU frame isn't opened here: the step runs while the GPU still draws the previous frame, and the first
        // renderer call that needs the GPU opens it (N3DSRenderer_frameGate), else the drawing below does. (The screen
        // captures here open it themselves when they run.)
        // Boot diagnostics: the previously presented frame is complete now (the harness screenshots open the gate the
        // same way), so this file is exactly what the player saw.
        if (bootPendingShot >= 0) {
            N3DSScreenshot_captureTopNow(renderer, bootPendingShot);
            bootPendingShot = -1;
        }
        N3DSScreenshot_captureIfRequested(renderer);
        N3DSLiveMap_beginFrame();
        // Before the step: follows the pause state. The pause menu itself is drawn by the game on the top screen and
        // is operated with its own buttons.
        (void) N3DSPause_update();
        bool frameOpenBeforeStep = N3DSRenderer_frameIsOpen();
        u64 stepStartTick = svcGetSystemTick();
        {
            N3DS_ZONE(N3DS_ZONE_STEP);
            Runner_step(runner);
            N3DSAm2r_update();
        }
#if defined(ENABLE_VM_GML_PROFILER) && !defined(N3DS_FRAME_PROFILER)
        if (runner->frameCount > 0 && runner->frameCount % 120 == 0) {
            char* report = Profiler_createReport(vm->profiler, 25, 120);
            if (report != NULL) {
                logInfo("%s\n", report);
                free(report);
            }
            Profiler_reset(vm->profiler);
        }
#endif
        float dt = (float) (runner->deltaTime / 1000000.0);
        if (0.0f > dt) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        runner->audioSystem->vtable->update(runner->audioSystem, dt);
        // The frameskip's other step(s), with their own input; not once a room change is pending (it happens after
        // the frame's drawing, below).
        for (int32_t extra = 1; extra < stepsPerFrame && runner->pendingRoom < 0 && !runner->shouldExit; extra++) {
            RunnerKeyboard_beginFrame(runner->keyboard);
            RunnerGamepad_beginFrame(runner->gamepads);
            RunnerMouse_beginFrame(runner->mouse);
            N3DSInput_update(runner);
            InputRecording_processFrame(inputPlayback, runner->keyboard, inputFrame++);
            if (N3DSInput_takeMonitorToggle()) debugMonitorVisible = !debugMonitorVisible;
            {
                N3DS_ZONE(N3DS_ZONE_STEP);
                Runner_step(runner);
                N3DSAm2r_update();
            }
            runner->audioSystem->vtable->update(runner->audioSystem, dt);
        }
        double stepMs = (double) (svcGetSystemTick() - stepStartTick) * 1000.0 / (double) SYSCLOCK_ARM11;
        bool frameOpenedInStep = !frameOpenBeforeStep && N3DSRenderer_frameIsOpen();
        N3DSRenderer_frameGate();
        double waitMs = N3DSRenderer_frameWaitMs();
        double gpuMs = N3DSRenderer_frameGpuMs();
        if (frameOpenedInStep) stepMs -= waitMs; // the wait happened inside the step: count it as wait
        N3DSRenderer_importReadAhead(renderer);

        bool roomChanged = runner->currentRoom != lastRoom;
        if (roomChanged) {
            // How the room just left ran, as a whole (the Perf line is per 5 s and mixes rooms).
            if (lastRoom != NULL && roomStats.frames > 0) {
                double n = (double) roomStats.frames;
                logInfo("Room perf: %s %lu frames, %.1f fps; avg step %.2f + wait %.2f + draw %.2f ms (frame %.2f), worst %.0f ms; "
                        "GPU drawing %.2f ms; avg %.0f GPU flushes, %.0f instances\n",
                    lastRoom->name, (unsigned long) roomStats.frames, n * 1000.0 / (roomStats.frameMs > 0.0 ? roomStats.frameMs : 1.0),
                    roomStats.stepMs / n, roomStats.waitMs / n, roomStats.drawMs / n, roomStats.frameMs / n, roomStats.worstMs,
                    roomStats.gpuMs / n, roomStats.flushes / n, roomStats.instances / n);
#ifdef N3DS_FRAME_PROFILER
                N3DS_logRoomProfile(lastRoom->name, roomStats.frames, roomStats.gpuMs, vm);
#endif
            }
            memset(&roomStats, 0, sizeof(roomStats));
            lastRoom = runner->currentRoom;
            logInfo("Room %d: %s (%dx%d, speed %d)\n", (int) runner->currentRoomIndex, runner->currentRoom->name,
                (int) runner->currentRoom->width, (int) runner->currentRoom->height, (int) runner->currentRoom->speed);
            u64 prewarmStart = N3DSProf_begin();
            N3DSRenderer_prewarmRoom(renderer, runner);
#ifdef N3DS_ENABLE_AUDIO
            N3DSAudio_prewarmRoom(runner->audioSystem, runner);
#endif
            N3DSProf_end(N3DS_PROF_PREWARM, prewarmStart);
            // The boot frames are not where the wrong picture is: it is the first frames of the title room, which start
            // seconds later. Restart the trace on every room entry so the room that draws it is the one recorded.
            if (fileExists(N3DS_SD_DIR "drawtrace.txt")) N3DSRenderer_startDrawTrace(renderer, 6);
            N3DS_logMemory("room start");
        }

        if (!runner->appSurfaceEnabled) {
            runner->applicationWidth = N3DS_TOP_SCREEN_W;
            runner->applicationHeight = N3DS_TOP_SCREEN_H;
            runner->usingAppSurface = false;
        } else {
            if (runner->applicationWidth <= 0 || runner->applicationHeight <= 0) {
                runner->applicationWidth = (int32_t) gen8->defaultWindowWidth;
                runner->applicationHeight = (int32_t) gen8->defaultWindowHeight;
            }
            runner->usingAppSurface = true;
        }
        int32_t gameW = runner->applicationWidth;
        int32_t gameH = runner->applicationHeight;

        // Screen mode (N3DS_DEFAULT_SCREEN_MODE). AM2R draws its own picture (a 426x240 widescreen view) into a
        // window it is told about, scaling and centring it itself, so the mode decides what window the game gets and
        // how the port then presents it:
        //   Wide:     the screen's window (400x240). The game fits its 426-wide view into that by itself (0.939) and
        //             centres it, and N3DSAm2r_applyScreenMode overrides that to 1:1 with the picture centred, so the
        //             426x240 picture is shown pixel-for-pixel with the 13 columns at each edge given up. No bars.
        //   1x:      the same window (400x240) with no override: the game's own 0.939 fit and centring stand, so the
        //             whole 426x240 view is shown at 400x225 with ~7px bars top and bottom. Slightly soft.
        //   2x:      the game's own window at 2x, centred: the middle of the picture, the rest off screen.
        //   Stretch: the game's own window (426), and the screen stretches that onto 400x240. The whole view, with the
        //             426 columns squeezed into 400 (a 0.939 horizontal squash).
        runner->widescreenExtraWidth = 0;
        runner->widescreenExtraHeight = 0;
        int32_t ownGameW = N3DSAm2r_gameWidth();
        bool wide = gScreenMode == N3DS_SCREEN_WIDE;
        if (wide && ownGameW == 0 && runner->usingAppSurface && gameW > 0 && gameH > 0) {
            int32_t targetW = (int32_t) ((float) gameH * ((float) N3DS_TOP_SCREEN_W / (float) N3DS_TOP_SCREEN_H) + 0.5f);
            if (targetW > gameW) {
                runner->widescreenExtraWidth = targetW - gameW;
                gameW = targetW;
            }
        }
        // The "window" the game sees. Stretch gives it a window as wide as its own picture, so it draws 1:1 and the
        // port squashes that onto the screen. Wide and point-for-point give it the screen instead, so it scales and
        // centres its picture itself (both then override that, see N3DSAm2r_applyScreenMode).
        bool stretch = gScreenMode == N3DS_SCREEN_STRETCH;
        int32_t fixedScale = 0;
        bool gameSizedWindow = stretch;
        gWindowW = gameSizedWindow ? (ownGameW > 0 ? ownGameW : runner->usingAppSurface ? runner->applicationWidth : (int32_t) gen8->defaultWindowWidth) : N3DS_TOP_SCREEN_W;
        gWindowH = gameSizedWindow ? (runner->usingAppSurface ? runner->applicationHeight : (int32_t) gen8->defaultWindowHeight) : N3DS_TOP_SCREEN_H;
        // The pause screen (rm_options) lays itself out for a 320x568 surface. In a game-sized window (stretch, 2x)
        // that would make the window 568 tall and letterbox the picture away, so the height is pinned to the screen.
        if (N3DSPause_isPaused()) gWindowH = N3DS_TOP_SCREEN_H;
        N3DSRenderer_setStretchToScreen(renderer, stretch);
        N3DSRenderer_setFixedScale(renderer, fixedScale);

        u64 drawStartTick = svcGetSystemTick();
        {
            N3DS_ZONE(N3DS_ZONE_FRAME_START);
            Runner_drawPre(runner, gWindowW, gWindowH);
            Runner_beginFrame(runner, gameW, gameH, gWindowW, gWindowH, gWindowW, gWindowH);
        }
        {
            N3DS_ZONE(N3DS_ZONE_VIEWS);
            Runner_drawViews(runner, gameW, gameH, false);
        }
        {
            N3DS_ZONE(N3DS_ZONE_POST);
            renderer->vtable->endFrameInit(renderer);
            Runner_drawPost(runner, gWindowW, gWindowH);
            renderer->vtable->endFrameEnd(renderer);
        }
        {
            N3DS_ZONE(N3DS_ZONE_GUI);
            // AM2R 1.5 draws its HUD in window coordinates (it centres a 426-wide picture in the window itself).
            Runner_drawGUI(runner, gWindowW, gWindowH, ownGameW > 0 ? gWindowW : gameW, ownGameW > 0 ? gWindowH : gameH);
        }
#ifdef N3DS_DIAG_PATTERN
        if (runner->frameCount % 300 == 5) N3DSRenderer_logDiag(renderer);
#endif
        {
            N3DS_ZONE(N3DS_ZONE_OVERLAYS);
            // The bottom screen: AM2R's map while a save is being played, otherwise it stays the black the frame
            // opened with. Drawn before the top screen's overlays so those keep the top screen's mapping.
            if (N3DSPause_inPlay() && N3DSLiveMap_available()) {
                N3DSLiveMap_draw();
                // After the draw, so the map this touch moves is the one just drawn: dragging pans it, a tap sets or
                // lifts the marker.
                N3DSPause_updateMapTouch();
            }
            if (debugMonitorVisible) N3DSDebugMonitor_draw(&debugMonitor, runner, renderer);
            if (gFpsCounter) N3DSFpsCounter_draw(&fpsCounter, renderer, debugMonitor.displayedFps);
            renderer->vtable->flush(renderer);
        }
        // After the frame's drawing (all counted as draw): the room change, the renderer's texture collection and the
        // game files' write-back, timed on their own for the slow-frame line.
        u64 roomChangeStartTick = svcGetSystemTick();
        {
            N3DS_ZONE(N3DS_ZONE_ROOM_CHANGE);
            Runner_handlePendingRoomChange(runner);
            renderer->vtable->flush(renderer);
        }
        double roomChangeMs = N3DSProf_ms(svcGetSystemTick() - roomChangeStartTick);
        if (bootBlackout) {
            const char* bootRoom = runner->currentRoom != NULL ? runner->currentRoom->name : NULL;
            if (bootRoom == NULL || strcmp(bootRoom, "initroom") == 0 || strcmp(bootRoom, "rm_transition") == 0) {
                // After the game's own drawing, so it covers whatever it drew on the top screen this frame.
                N3DSRenderer_clearScreens(renderer);
            } else {
                bootBlackout = false;
                logInfo("N3DS: boot blackout ends in %s\n", bootRoom);
            }
        }
        if (bootShotsLeft > 0 && bootPendingShot < 0) {
            bootShotsLeft--;
            bootPendingShot = bootShotIndex++;
        }
        N3DSRenderer_frameEnd();
        u64 gcStartTick = svcGetSystemTick();
#ifdef N3DS_FRAME_PROFILER
        int afterFrameZone = N3DSZone_enter(N3DS_ZONE_AFTER_FRAME);
#endif
        N3DSRenderer_collectGarbage(renderer, false);
        double gcMs = N3DSProf_ms(svcGetSystemTick() - gcStartTick);
        u64 fsFlushStartTick = svcGetSystemTick();
        N3DSCachedFileSystem_flush(fileSystem, false);
#ifdef N3DS_FRAME_PROFILER
        N3DSZone_exit(&afterFrameZone);
#endif
        double fsFlushMs = N3DSProf_ms(svcGetSystemTick() - fsFlushStartTick);
        u64 frameEndTick = svcGetSystemTick();
        double drawMs = N3DSProf_ms(frameEndTick - drawStartTick);
        double frameTimes[N3DS_FT_COUNT] = {
            [N3DS_FT_FRAME] = N3DSProf_ms(frameEndTick - frameStartTick),
            [N3DS_FT_STEP] = stepMs,
            [N3DS_FT_WAIT] = waitMs,
            [N3DS_FT_DRAW] = drawMs,
            [N3DS_FT_IO] = N3DSProf_ms(gN3DSProfTicks[N3DS_PROF_IO]),
            [N3DS_FT_TEX] = N3DSProf_ms(gN3DSProfTicks[N3DS_PROF_TEX]),
            [N3DS_FT_FS] = N3DSProf_ms(gN3DSProfTicks[N3DS_PROF_FS]),
            [N3DS_FT_AUDIO] = N3DSProf_ms(gN3DSProfTicks[N3DS_PROF_AUDIO]),
            [N3DS_FT_PREWARM] = N3DSProf_ms(gN3DSProfTicks[N3DS_PROF_PREWARM]),
        };
        N3DSDebugMonitor_tickFrame(&debugMonitor, frameTimes);
        if (frameTimes[N3DS_FT_FRAME] > statsMaxFrameMs) statsMaxFrameMs = frameTimes[N3DS_FT_FRAME];
        // The room's first frame (loading) is left out of its averages.
        if (!roomChanged) {
            roomStats.frames++;
            roomStats.stepMs += stepMs;
            roomStats.waitMs += waitMs;
            roomStats.drawMs += drawMs;
            roomStats.frameMs += frameTimes[N3DS_FT_FRAME];
            roomStats.gpuMs += gpuMs;
            roomStats.flushes += (double) gN3DSProfFlushes;
            roomStats.instances += (double) arrlen(runner->instances);
            if (frameTimes[N3DS_FT_FRAME] > roomStats.worstMs) roomStats.worstMs = frameTimes[N3DS_FT_FRAME];
        }
        // A frame over 25 ms (under 40 fps) is a hitch or a slowdown: say where it went (at most twice a second, so a
        // long stall doesn't turn into SD writes of its own).
        u64 frameEndMs = osGetTime();
        if (roomChanged || (frameTimes[N3DS_FT_FRAME] > 25.0 && frameEndMs - lastSlowLogMs >= 500u)) {
            if (!roomChanged) lastSlowLogMs = frameEndMs;
            logInfo("%s %d (%s): %.1f ms = step %.1f + wait %.1f + draw %.1f (room change %.1f, texture gc %.1f, file flush %.1f) + prewarm %.1f; io %.1f, tex %.1f, fs %.1f, audio %.1f; %lu instances, %lu GPU flushes\n",
                roomChanged ? "Room frame" : "Slow frame", (int) runner->frameCount,
                runner->currentRoom != NULL ? runner->currentRoom->name : "-",
                frameTimes[N3DS_FT_FRAME], stepMs, waitMs, drawMs, roomChangeMs, gcMs, fsFlushMs, frameTimes[N3DS_FT_PREWARM],
                frameTimes[N3DS_FT_IO], frameTimes[N3DS_FT_TEX], frameTimes[N3DS_FT_FS], frameTimes[N3DS_FT_AUDIO],
                (unsigned long) arrlen(runner->instances), (unsigned long) gN3DSProfFlushes);
        }

        if (N3DSInput_exitRequested()) {
            FILE* done = fopen(N3DS_SD_DIR "done.txt", "w");
            if (done != NULL) {
                fprintf(done, "frame %d room %s\n", (int) runner->frameCount, runner->currentRoom != NULL ? runner->currentRoom->name : "-");
                fclose(done);
            }
            break;
        }

        N3DSLog_tick();

        // Deepest stack pointer seen this session. A crash dump caught the main thread storing to the stack with SP at
        // 0x080070d0 and the faulting address nowhere near it; if the stack really is the problem, this number walks
        // towards that value first, and the Perf line shows it before the crash.
        {
            uint32_t frameSp;
            __asm__ volatile("mov %0, sp" : "=r"(frameSp));
            if (gStackLowest == 0u || frameSp < gStackLowest) gStackLowest = frameSp;
        }

        statsFrames++;
        statsStepMs += stepMs;
        statsDrawMs += drawMs;
        statsWaitMs += waitMs;
        statsSprites += N3DSRenderer_getFrameSpriteDrawCalls(renderer);
        statsParts += N3DSRenderer_getFrameSpritePartDrawCalls(renderer);
        statsFrags += N3DSRenderer_getFrameFragmentDraws(renderer);
        statsGlyphs += N3DSRenderer_getFrameTextGlyphDraws(renderer);
        statsFlushes += gN3DSProfFlushes;
        u64 nowMs = osGetTime();
        if (nowMs - statsStartMs >= 5000u) {
            double seconds = (double) (nowMs - statsStartMs) / 1000.0;
            // The heap reserves the whole app region up front, so report what malloc actually uses.
            struct mallinfo mi = mallinfo();
            uint32_t effects = 0;
#ifdef N3DS_ENABLE_AUDIO
            effects = N3DSAudio_effectsStarted(runner->audioSystem);
#endif
            logInfo("Perf: %.1f fps, step %.2f ms, draw %.2f ms, gpu/vblank wait %.2f ms (avg over %u frames, worst %.0f ms), room %s, heap used %lu KB, heap free %lu KB, heap arena %lu KB, linear free %lu KB, stack low 0x%08lx, sfx %lu, draw sprites %.1f parts %.1f frags %.1f glyphs %.1f flushes %.1f\n",
                (double) statsFrames / seconds, statsStepMs / statsFrames, statsDrawMs / statsFrames, statsWaitMs / statsFrames, (unsigned) statsFrames, statsMaxFrameMs,
                runner->currentRoom != NULL ? runner->currentRoom->name : "-",
                (unsigned long) (mi.uordblks / 1024u), (unsigned long) (mi.fordblks / 1024u),
                (unsigned long) (mi.arena / 1024u), (unsigned long) (linearSpaceFree() / 1024u), (unsigned long) gStackLowest, (unsigned long) effects,
                (double) statsSprites / statsFrames, (double) statsParts / statsFrames, (double) statsFrags / statsFrames,
                (double) statsGlyphs / statsFrames, (double) statsFlushes / statsFrames);
            statsStartMs = nowMs;
            statsFrames = 0;
            statsStepMs = 0.0;
            statsDrawMs = 0.0;
            statsWaitMs = 0.0;
            statsSprites = 0;
            statsParts = 0;
            statsFrags = 0;
            statsGlyphs = 0;
            statsFlushes = 0;
            statsMaxFrameMs = 0.0;
            if (N3DS_unimplCount() != lastUnimplCount) {
                lastUnimplCount = N3DS_unimplCount();
                N3DS_unimplDump("periodic");
            }
        }
    }

    N3DS_unimplDump("exit");
    logInfo("Exiting\n");
    N3DSDebugMonitor_free(&debugMonitor);
    audioSystem->vtable->destroy(audioSystem);
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    OverlayFileSystem_destroy((OverlayFileSystem*) N3DSCachedFileSystem_destroy(fileSystem));
    VM_free(vm);
    DataWin_free(dataWin);
    N3DSLog_close();
    if (citroReady) {
        if (N3DSRenderer_frameIsOpen()) N3DSRenderer_frameEnd(); // opened by shutdown code, if any
        C2D_Fini();
        C3D_Fini();
    }
    aptUnhook(&gN3DSAptCookie);
    romfsExit();
    gfxExit();
    return 0;
}
