#include "diag.h"

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#define DIAG_DIR  "sdmc:/3ds/skywave"
#define DIAG_PATH DIAG_DIR "/diag.log"

// Restarted rather than grown past this. 64 KB is hundreds of tune-ins - far
// more history than any report needs - and it bounds what a runaway loop can do
// to someone's SD card. Truncating from the front would be kinder but needs a
// rewrite of the whole file on every append; a user chasing a bug re-runs it,
// so the newest session is the one that matters and starting over is fine.
#define DIAG_MAX_BYTES (64 * 1024)

static bool       s_ready;
static LightLock  s_lock;
static u64        s_t0;

// The lock is created HERE, in an init that main() calls before any other
// thread exists, and never lazily inside swDiagf(). A lock created by the call
// it is supposed to guard leaves the first call - the interesting one, the one
// that races - unprotected, because the lock does not exist yet at the moment
// two threads first reach it.
void swDiagInit(const char *version)
{
    LightLock_Init(&s_lock);
    s_t0    = osGetTime();
    s_ready = true;

    mkdir(DIAG_DIR, 0777);   // harmless if it already exists

    // Truncate on session start: "w", not "a". Each run gets a clean file, so a
    // user who reproduces a bug and then sends the log is sending THAT run.
    FILE *f = fopen(DIAG_PATH, "w");
    if (!f) {
        // Nothing to be done and nothing worth failing over - the app runs fine
        // without a log. s_ready stays true so later calls retry the open; an
        // SD card that is busy now may be writable in a second.
        return;
    }
    fprintf(f, "Skywave %s diagnostic log\n", version ? version : "?");
    fprintf(f, "Times are milliseconds since this line.\n\n");
    fclose(f);
}

void swDiagf(const char *fmt, ...)
{
    if (!s_ready) return;

    LightLock_Lock(&s_lock);

    FILE *f = fopen(DIAG_PATH, "a");
    if (f) {
        // Cap check on the handle we already hold, rather than a separate stat:
        // one syscall fewer, and no window where the file changes size between
        // the two calls.
        fseek(f, 0, SEEK_END);
        if (ftell(f) > DIAG_MAX_BYTES) {
            fclose(f);
            f = fopen(DIAG_PATH, "w");
            if (f) fprintf(f, "[log restarted at %d KB]\n", DIAG_MAX_BYTES / 1024);
        }
    }

    if (f) {
        fprintf(f, "%6llu  ", (unsigned long long)(osGetTime() - s_t0));

        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);

        fputc('\n', f);
        fclose(f);   // closed every time: a log that is still buffered when the
                     // console freezes has recorded nothing, and freezing is one
                     // of the things this is here to catch.
    }

    LightLock_Unlock(&s_lock);
}

void swDiagExit(void)
{
    swDiagf("app exit (clean)");
    s_ready = false;
}
