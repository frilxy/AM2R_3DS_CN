#include "../log.h"
#include "n3ds_platform_config.h"

#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

// platformLog for the 3DS: N3DS_SD_DIR "log.txt", each line stamped with seconds since start.
// Also sent to svcOutputDebugString (shown by Azahar / GDB).
// Each flush is an SD write (milliseconds), so lines are flushed only every so often (N3DSLog_tick, every frame) or
// right away for errors; and a warning logged over and over (inside a loop) stops after a few dozen times.
//
// 250 ms rather than a second: a hang or a hardware crash loses whatever was still buffered, and the last second is
// exactly the interesting part. `fsync` makes the write survive a power cycle, not just the process.
#define N3DS_LOG_FLUSH_INTERVAL_MS 250u
#define N3DS_LOG_REPEAT_LIMIT 32u
#define N3DS_LOG_REPEAT_SLOTS 64

static FILE* gLogFile = NULL;
// This session's file: log-YYYYMMDD-HHMMSS.txt. One file per launch means a copy taken after playing can never be an
// older session's, which is what made "the log never changes" impossible to argue with.
static char gSessionLogName[64] = "log.txt";
static char gSessionLogPath[80] = N3DS_SD_DIR "log.txt";
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

// Copies the previous session's log to log_crash.txt when that session did not exit cleanly. A copy rather than a
// rename: rename() has been seen to leave the old file in place here (the card kept handing back a stale
// log_crash.txt), and a copy also leaves log_prev.txt holding the same session as a second chance.
static uint32_t N3DSLog_copyCrashedLog(const char* path) {
    FILE* in = fopen(path, "rb");
    if (in == NULL) return 0;
    remove(N3DS_SD_DIR "log_crash_prev.txt"); // the two-copy scheme is gone; keep the folder tidy
    FILE* out = fopen(N3DS_SD_DIR "log_crash.txt", "wb");
    uint32_t total = 0;
    if (out != NULL) {
        char buffer[4096];
        size_t read;
        while ((read = fread(buffer, 1, sizeof buffer, in)) > 0) {
            if (fwrite(buffer, 1, read, out) != read) break;
            total += (uint32_t) read;
        }
        fflush(out);
        fsync(fileno(out));
        fclose(out);
    }
    fclose(in);
    return total;
}

// True when the previous session's log ends with the runner's own "Exiting" line, i.e. it shut down cleanly. Anything
// else (a hang, a GPU fault, the user pulling the power) means that file is the one worth keeping.
static bool N3DSLog_previousRunExitedCleanly(const char* path) {
    FILE* previous = fopen(path, "rb");
    if (previous == NULL) return true; // nothing to keep
    bool clean = false;
    if (fseek(previous, 0, SEEK_END) == 0) {
        long size = ftell(previous);
        long tail = size > 512 ? 512 : size;
        if (size > 0 && fseek(previous, size - tail, SEEK_SET) == 0) {
            char buffer[513];
            size_t read = fread(buffer, 1, (size_t) tail, previous);
            buffer[read] = '\0';
            clean = strstr(buffer, "Exiting") != NULL;
        }
    }
    fclose(previous);
    return clean;
}

void N3DSLog_init(void) {
    LightLock_Init(&gLogLock);
    gLogReady = true;
    gLogStartMs = osGetTime();
    gLastFlushMs = gLogStartMs;

    // The previous session's file name is kept in log_last.txt, because the name now carries the time.
    char previousLog[80] = N3DS_SD_DIR "log.txt";
    FILE* lastName = fopen(N3DS_SD_DIR "log_last.txt", "r");
    if (lastName != NULL) {
        char line[64];
        if (fgets(line, sizeof line, lastName) != NULL) {
            size_t n = strlen(line);
            while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
            if (n > 4 && strncmp(line, "log-", 4) == 0) {
                snprintf(previousLog, sizeof previousLog, N3DS_SD_DIR "%s", line);
            }
        }
        fclose(lastName);
    }
    // A session that did not exit cleanly is kept as log_crash.txt: the player relaunches before copying anything, and
    // with one file per launch the crashed one is still on the card under its own name anyway.
    uint32_t keptCrashBytes = 0;
    if (!N3DSLog_previousRunExitedCleanly(previousLog)) keptCrashBytes = N3DSLog_copyCrashedLog(previousLog);

    time_t now = (time_t) (osGetTime() / 1000);
    struct tm* tm = localtime(&now);
    if (tm != NULL) {
        snprintf(gSessionLogName, sizeof gSessionLogName, "log-%04d%02d%02d-%02d%02d%02d.txt",
            tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    } else {
        snprintf(gSessionLogName, sizeof gSessionLogName, "log-%lu.txt", (unsigned long) (osGetTime() / 1000));
    }
    snprintf(gSessionLogPath, sizeof gSessionLogPath, N3DS_SD_DIR "%s", gSessionLogName);
    gLogFile = fopen(gSessionLogPath, "w");
    FILE* lastNameOut = fopen(N3DS_SD_DIR "log_last.txt", "w");
    if (lastNameOut != NULL) {
        fprintf(lastNameOut, "%s\n", gSessionLogName);
        fclose(lastNameOut);
    }
    // Say so in the new log: it is the only way to tell from the outside whether the crashed session was preserved.
    if (keptCrashBytes > 0) {
        logInfo("Previous session did not exit cleanly; copied %lu KB of its log to log_crash.txt\n",
            (unsigned long) (keptCrashBytes / 1024u));
    }
}

void N3DSLog_close(void) {
    if (gLogFile != NULL) fclose(gLogFile);
    gLogFile = NULL;
}

static void N3DSLog_flushLocked(u64 nowMs) {
    if (gLogFile != NULL && gLogDirty) {
        fflush(gLogFile);
        // Push it past the FS layer too: a crash (or the user holding POWER) otherwise loses what is still in the card
        // service's cache, which is how a crashed session ended up with nothing worth reading.
        fsync(fileno(gLogFile));
    }
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
