#include "n3ds_input.h"
#include "n3ds_platform_config.h"
#include "n3ds_renderer.h"

#include "../log.h"
#include "../utils.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "stb_image_write.h"

// Debug/verification only: copies the finished screens' color buffers (VRAM, 8x8 Morton tiles, RGBA8, rotated 90
// degrees) into PNGs (frame_<n>.png for the top screen, frame_<n>_bottom.png for the bottom one). Not on the draw
// path.

static int32_t gRequestedFrame = -1;

void N3DSScreenshot_request(int32_t frame) {
    gRequestedFrame = frame;
}

static inline uint32_t N3DSScreenshot_morton(uint32_t x, uint32_t y) {
    return (x & 1u) | ((y & 1u) << 1) | ((x & 2u) << 1) | ((y & 2u) << 2) | ((x & 4u) << 2) | ((y & 4u) << 3);
}

static void N3DSScreenshot_save(C3D_RenderTarget* target, const char* path) {
    if (target == NULL || target->frameBuf.colorBuf == NULL) return;
    const uint32_t fbW = target->frameBuf.width;   // 240 (the screen is rotated)
    const uint32_t fbH = target->frameBuf.height;  // 400
    const uint32_t outW = fbH, outH = fbW;
    const uint32_t* src = (const uint32_t*) target->frameBuf.colorBuf;
    uint8_t* rgb = (uint8_t*) malloc((size_t) outW * outH * 3u);
    if (rgb == NULL) return;

    for (uint32_t sy = 0; sy < outH; sy++) {
        for (uint32_t sx = 0; sx < outW; sx++) {
            uint32_t fx = (fbW - 1u) - sy;
            uint32_t fy = sx;
            uint32_t tile = (fy >> 3) * (fbW >> 3) + (fx >> 3);
            uint32_t p = src[tile * 64u + N3DSScreenshot_morton(fx & 7u, fy & 7u)];
            uint8_t* o = &rgb[((size_t) sy * outW + sx) * 3u];
            o[0] = (uint8_t) (p >> 24);
            o[1] = (uint8_t) (p >> 16);
            o[2] = (uint8_t) (p >> 8);
        }
    }

    if (stbi_write_png(path, (int) outW, (int) outH, 3, rgb, (int) outW * 3)) {
        logInfo("Screenshot: %s\n", path);
    } else {
        logWarn("Screenshot: could not write %s\n", path);
    }
    free(rgb);
}

void N3DSScreenshot_captureIfRequested(Renderer* renderer) {
    if (gRequestedFrame < 0) return;
    int32_t frame = gRequestedFrame;
    gRequestedFrame = -1;
    N3DSRenderer_frameGate(); // the last finished frame, once the GPU is done with it
    mkdir(N3DS_SD_DIR "shots", 0777);
    char path[128];
    snprintf(path, sizeof(path), N3DS_SD_DIR "shots/frame_%05d.png", (int) frame);
    N3DSScreenshot_save(N3DSRenderer_getTopTarget(renderer), path);
    // tools/n3ds/compare_rooms.py pairs each screenshot with the room it was taken in from this line.
    logInfo("Screenshot saved: %s\n", path);
    snprintf(path, sizeof(path), N3DS_SD_DIR "shots/frame_%05d_bottom.png", (int) frame);
    N3DSScreenshot_save(N3DSRenderer_getBottomTarget(renderer), path);
}
