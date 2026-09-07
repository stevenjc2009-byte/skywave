// Skywave - internet radio for the Nintendo 3DS.
//
// This file does nothing but bring the console up in the right order, hand over
// to the app, and put it back down again. The order genuinely matters and each
// step is commented with why, because getting it wrong produces failures that
// look like something else entirely.

#include <3ds.h>
#include <stdio.h>

#include "app/app.h"
#include "audio/player.h"
#include "net/tcp.h"
#include "store/diag.h"
#include "ui/ui.h"
#include "version.h"

// Draws a full-screen apology on the console text output and waits for START.
// Used only for failures that happen before the real UI exists - at that point
// there is no citro2d, so this is the console, and it is still far better than
// exiting silently to the HOME menu with no explanation.
static void fatal(const char *what, const char *detail)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    printf("\x1b[2J");
    printf("\x1b[1;2HSkywave could not start.\n\n");
    printf("  %s\n\n", what);
    if (detail && *detail) printf("  %s\n\n", detail);
    printf("\x1b[27;2HPress START to exit.");

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    gfxExit();
}

int main(void)
{
    // Run at full speed on a New 3DS. Harmless on an Old one, where it does
    // nothing, and the decode thread has enough headroom either way - but a
    // free 268 -> 804 MHz on the consoles that have it is worth taking.
    osSetSpeedupEnable(true);

    // Opened first, so that every line below is recorded - including the
    // failures that take the fatal() path and exit before the UI has drawn a
    // single frame. See store/diag.h for why a shipped build carries this.
    swDiagInit(SKYWAVE_VERSION);

    // This is what makes the network thread possible. Core 1 belongs to the
    // system; without this call, threadCreate on core 1 fails and every read
    // ends up competing with the decoder and the UI on core 0. It is one-way -
    // there is no putting it back - which is why it happens here, once, rather
    // than somewhere that might run twice.
    APT_SetAppCpuTimeLimit(30);

    // Holds one file: the certificate authority bundle at romfs:/cacert.pem.
    // Not fatal if it fails - the trust store is also looked for on the SD card,
    // and streams do not verify at all, so the only thing lost is the updater.
    bool have_romfs = R_SUCCEEDED(romfsInit());
    swDiagf("romfs=%d", (int)have_romfs);

    // Sockets, the random number generator and TLS. This replaced httpcInit: the
    // app no longer uses httpc for anything, because ssl:C underneath it is
    // fixed at TLS 1.1 and most streaming hosts - and every GitHub host - refuse
    // a handshake in that shape. See net/tcp.h for the full reasoning.
    //
    // Still fatal, for the same reason httpcInit was: everything this app does
    // is over the network.
    if (!swNetInit()) {
        swDiagf("swNetInit FAILED - fatal, exiting");
        fatal("The network service would not start.",
              "Try restarting the console.");
        if (have_romfs) romfsExit();
        return 0;
    }
    swDiagf("swNetInit ok");

    // Needed only by the updater, to install a downloaded CIA. A failure here
    // is not fatal - everything except updating still works - so it is noted
    // and carried on from rather than stopping the app.
    bool have_am = R_SUCCEEDED(amInit());
    swDiagf("am=%d", (int)have_am);

    // A failure here does NOT stop the app. It used to, on the reasoning that
    // sound is the point of a radio - but that reasoning has a hole in it: the
    // one thing a console without dspfirm.cdc most needs is the in-app updater,
    // and exiting here puts the updater out of reach along with everything
    // else. So the reason is carried into the app, which stays fully usable and
    // says why there is no sound the moment the user asks for any.
    SwPlayerInitResult pr = swPlayerInit();
    swDiagf("swPlayerInit -> %d (%s)", (int)pr,
            pr == SW_PLAYER_OK ? "OK" : swPlayerInitTextShort(pr));

    if (!swUiInit()) {
        fatal("The graphics system would not start.", NULL);
        swPlayerExit();
        if (have_am) amExit();
        swNetExit();
        if (have_romfs) romfsExit();
        return 0;
    }

    swAppRun(pr);

    // Torn down in the reverse of the order it came up in. The player stops its
    // threads before the graphics go away, because a thread still running when
    // the process exits is a hang rather than a crash and is much harder to
    // diagnose from a user's description.
    swDiagf("swAppRun returned - shutting down");

    swUiExit();
    swPlayerExit();
    if (have_am) amExit();
    swNetExit();
    if (have_romfs) romfsExit();

    // Last line written. A log that does NOT end here was killed, froze or
    // crashed rather than exiting - which is itself the answer to a whole class
    // of "it just stopped" reports.
    swDiagExit();

    return 0;
}
