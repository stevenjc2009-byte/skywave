#pragma once

// Playback: one URL in, sound out of the speakers.
//
// Three things are happening at once and they run at wildly different speeds,
// which is why this is threaded rather than a loop:
//
//   * The network delivers a 128 kbps stream in bursts - sometimes 8 KB at
//     once, sometimes nothing for half a second while wifi does something else.
//   * The decoder wants to work in whole MPEG frames, 1152 samples at a time.
//   * The DSP wants a buffer handed to it every ~90 ms, on time, forever. Miss
//     one and the user hears a click.
//
// So: a network thread fills a ring buffer as fast as the station sends, and a
// decode thread drains it at exactly the rate the DSP consumes. The ring is the
// shock absorber between them - about eight seconds of it, which is what makes
// the difference between a wifi hiccup being inaudible and being a dropout.
//
// PREREQUISITE: ndsp needs `sdmc:/3ds/dspfirm.cdc`, dumped from the console by
// the user with DSP1. There is no way around this and no way to ship it. If it
// is missing, swPlayerInit fails with SW_PLAYER_NO_DSPFIRM and the app must say
// so in words - an app that silently plays nothing is an app that looks broken.

#include <stdbool.h>

#include "../net/directory.h"

typedef enum {
    SW_PLAY_STOPPED = 0,
    SW_PLAY_CONNECTING,   // opening the stream, reading headers
    SW_PLAY_BUFFERING,    // connected, filling the ring before the first sound
    SW_PLAY_PLAYING,
    SW_PLAY_ERROR         // gave up; swPlayerError() says why
} SwPlayState;

typedef enum {
    SW_PLAYER_OK = 0,
    SW_PLAYER_NO_DSPFIRM,   // sdmc:/3ds/dspfirm.cdc is missing
    SW_PLAYER_NO_NDSP,      // the service is there but would not start
    SW_PLAYER_NO_MEMORY
} SwPlayerInitResult;

// Starts the DSP and allocates the buffers. Call once at startup.
SwPlayerInitResult swPlayerInit(void);
void               swPlayerExit(void);

// Human-readable text for a failed init, for putting on screen.
const char *swPlayerInitText(SwPlayerInitResult r);

// Begins playing a station. Returns immediately - connecting happens on the
// network thread, so the UI never freezes on a station that is slow to answer.
// Calling this while something is already playing stops that first.
bool swPlayerPlay(const SwStation *st);

// Stops and tears down the threads. Blocks until they are gone, which is
// bounded: the network read is cancelled rather than waited on.
void swPlayerStop(void);

SwPlayState swPlayerState(void);

// The station currently loaded, or NULL when stopped. Valid until the next
// swPlayerPlay/swPlayerStop.
const SwStation *swPlayerStation(void);

// Now-playing text from the stream's ICY metadata, or "" if the station sends
// none. Copies into the caller's buffer because the source is written by
// another thread.
void swPlayerNowPlaying(char *dst, size_t cap);

// Why playback stopped, when the state is SW_PLAY_ERROR. "" otherwise.
const char *swPlayerError(void);

// How full the ring is, 0..100. This is the honest health indicator: a station
// that keeps this near full is fine, one that sits near empty is about to
// stutter, and showing it means a dropout looks like a weak connection rather
// than a broken app.
int swPlayerBufferPercent(void);

// Volume, 0..100. Applied immediately and remembered across stations.
void swPlayerSetVolume(int percent);
int  swPlayerVolume(void);
