#pragma once

#include <3ds/types.h>
#include <stdbool.h>
#include <stdint.h>

#ifndef RUNNER_DEFINED
#define RUNNER_DEFINED
typedef struct Runner Runner;
#endif
typedef struct Renderer Renderer;

void N3DSInput_init(Runner* runner);
// Reads the buttons (plus harness.txt presses) into the keyboard and gamepad slot 0. Returns newly pressed keys.
u32 N3DSInput_update(Runner* runner);
bool N3DSInput_exitRequested(void);
// True once per Start+Select chord (the chord never reaches the game).
bool N3DSInput_takeMonitorToggle(void);
// True on the frame a Start press goes into the chord delay line: the game gets it N3DS_CHORD_FRAMES frames later.
bool N3DSInput_startComing(void);

// The touch screen this frame (from the hardware or the harness): held, and where (bottom-screen pixels).
bool N3DSInput_touch(int32_t* x, int32_t* y);
// Holds gamepad slot 0 button `index` (0-based, gp_face1 = 0) for this frame on top of the real buttons. Call after
// N3DSInput_update and before the game's step.
void N3DSInput_injectButton(Runner* runner, int32_t index);

// Self-screenshots of the top screen (PNG under N3DS_SD_DIR "shots/").
void N3DSScreenshot_request(int32_t frame);
// Call right after C3D_FrameBegin: the previous frame is finished and still in the top screen's color buffer.
void N3DSScreenshot_captureIfRequested(Renderer* renderer);
