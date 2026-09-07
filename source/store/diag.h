#pragma once

// A one-file flight recorder for the playback path.
//
// Why this exists. v1.0.3 shipped with a failure nobody could name: on real
// hardware no station would play, the top screen said "Stopped", and there was
// no second line of text at all. Every candidate was chased through the source
// and every one of them was refuted - the A press reaches the play handler, the
// handler sets a message, and the message is drawn (all three OBSERVED in the
// emulator, not argued from the code). The failure is somewhere inside playback,
// and playback is the one path that cannot run on a development machine: it
// needs sdmc:/3ds/dspfirm.cdc, which is a console's own DSP dump and cannot be
// redistributed or synthesised.
//
// So the console has to do the reporting. This appends plain-text lines to
// sdmc:/3ds/skywave/diag.log, next to the favourites file, and the answer to
// "why did nothing play" becomes a file a user can send rather than a screen
// they have to describe.
//
// Deliberately NOT a debug-build-only feature. A diagnostic that is compiled out
// of the shipped build is a diagnostic that is never there when it is needed -
// and the one build that failed is the one that shipped.
//
// Cost: an fopen/fprintf/fclose per EVENT, never per read or per frame. A whole
// tune-in writes about a dozen lines. The file is capped (see DIAG_MAX_BYTES in
// diag.c) and restarted rather than grown without limit, so it cannot fill a
// user's SD card if something loops.

#include <stdbool.h>

// Opens the log and writes a session banner. Safe to call once, from main,
// after romfs/sdmc are available. The lock this uses is created here, before
// any thread that writes to it exists - see the note in diag.c.
void swDiagInit(const char *version);

// Appends one line, with a millisecond timestamp taken at the call. printf
// formatting. Silently does nothing if swDiagInit was never called or the SD
// card is not writable: a diagnostic must never be the thing that breaks a run.
void swDiagf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Marks a clean shutdown, so a log whose last line is NOT this one is evidence
// the app was killed, froze, or crashed rather than exited.
void swDiagExit(void);
