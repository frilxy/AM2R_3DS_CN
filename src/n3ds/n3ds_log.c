#include "../log.h"
#include "n3ds_platform_config.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>

// platformLog for the 3DS: N3DS_SD_DIR "log.txt", each line stamped with seconds since start.
// Also sent to svcOutputDebugString (shown by Azahar / GDB).
// Each flush is an SD write (milliseconds), so lines are flushed at most once a second (N3DSLog_tick, every frame) or
// right away for errors; and a warning logged over and over (inside a loop) stops after a few dozen times.

#define N3DS_LOG_FLUSH_INTERVAL_MS 1000u
#define N3DS_LOG_REPEAT_LIMIT 32u
#define N3DS_LOG_REPEAT_SLOTS 64

static FILE* gLogFile = NULL;
static u64 gLogStartMs = 0;
static bool gAtLineStart = true;
static bool gLogDirty = false;
static u64 gLastFlushMs = 0;
static LightLock gLogLock;
static bool gLogReady = false;

// How many times each format string has been logged (by pointer: the formats are string literals).
static struct {
    const char* format;
    uint32_t count;
} gRepeats[N3DS_LOG_REPEAT_SLOTS];

void N3DSLog_init(void) {
    LightLock_Init(&gLogLock);
    gLogReady = true;
    gLogStartMs = osGetTime();
    gLastFlushMs = gLogStartMs;
    // The last run's log survives one relaunch (players restart before they think of copying it).
    remove(N3DS_SD_DIR "log_prev.txt");
    rename(N3DS_SD_DIR "log.txt", N3DS_SD_DIR "log_prev.txt");
    gLogFile = fopen(N3DS_SD_DIR "log.txt", "w");
}

void N3DSLog_close(void) {
    if (gLogFile != NULL) fclose(gLogFile);
    gLogFile = NULL;
}

static void N3DSLog_flushLocked(u64 nowMs) {
    if (gLogFile != NULL && gLogDirty) fflush(gLogFile);
    gLogDirty = false;
    gLastFlushMs = nowMs;
}

void N3DSLog_tick(void) {
    if (!gLogReady || !gLogDirty) return;
    u64 nowMs = osGetTime();
    if (nowMs - gLastFlushMs < N3DS_LOG_FLUSH_INTERVAL_MS) return;
    LightLock_Lock(&gLogLock);
    N3DSLog_flushLocked(nowMs);
    LightLock_Unlock(&gLogLock);
}

// 0 = log it, 1 = log it with a note that the rest are dropped, 2 = drop it.
static int N3DSLog_repeatState(const char* format) {
    for (int i = 0; i < N3DS_LOG_REPEAT_SLOTS; i++) {
        if (gRepeats[i].format == format) {
            uint32_t n = ++gRepeats[i].count;
            return n < N3DS_LOG_REPEAT_LIMIT ? 0 : n == N3DS_LOG_REPEAT_LIMIT ? 1 : 2;
        }
        if (gRepeats[i].format == NULL) {
            gRepeats[i].format = format;
            gRepeats[i].count = 1;
            return 0;
        }
    }
    return 0;
}

void platformLog(const logType type, const char* format, va_list va) {
    char buffer[1024];
    const char* prefix = "";
    switch (type) {
        case LOG_TYPE_WARNING: prefix = "Warning: "; break;
        case LOG_TYPE_ERROR: prefix = "Error: "; break;
        case LOG_TYPE_DEBUG: prefix = "Debug: "; break;
        default: break;
    }
    if (gLogReady) LightLock_Lock(&gLogLock);
    int repeat = type == LOG_TYPE_WARNING ? N3DSLog_repeatState(format) : 0;
    if (repeat == 2) {
        if (gLogReady) LightLock_Unlock(&gLogLock);
        return;
    }
    int written = snprintf(buffer, sizeof(buffer), "%s", prefix);
    vsnprintf(buffer + written, sizeof(buffer) - (size_t) written, format, va);
    buffer[sizeof(buffer) - 1] = '\0';

    svcOutputDebugString(buffer, (s32) strlen(buffer));
    if (gLogFile != NULL) {
        if (gAtLineStart) fprintf(gLogFile, "[%8.3f] ", (double) (osGetTime() - gLogStartMs) / 1000.0);
        fputs(buffer, gLogFile);
        size_t len = strlen(buffer);
        gAtLineStart = len > 0 && buffer[len - 1] == '\n';
        if (repeat == 1) {
            if (!gAtLineStart) fputc('\n', gLogFile);
            fprintf(gLogFile, "[%8.3f] (logged %u times: further copies of this message are dropped)\n",
                (double) (osGetTime() - gLogStartMs) / 1000.0, (unsigned) N3DS_LOG_REPEAT_LIMIT);
            gAtLineStart = true;
        }
        gLogDirty = true;
        if (type == LOG_TYPE_ERROR) N3DSLog_flushLocked(osGetTime());
    }
    if (gLogReady) LightLock_Unlock(&gLogLock);
}
