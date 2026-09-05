#pragma once

// HTTP for Skywave, on top of libctru's httpc service.
//
// Two very different jobs share this file:
//
//   * One-shot GETs of a few tens of KB - the station directory. Ordinary.
//
//   * An endless GET that is never meant to finish - a radio stream. This is
//     the awkward one. httpc is built around a response that ends, and
//     `httpcCloseContext` is documented to hang if the body was not fully
//     downloaded, which for a stream is always. The way round it is to cancel
//     the connection before closing the context, which is what swHttpClose
//     does and why closing a stream is not just a close.
//
// Redirects are followed by hand because httpc does not follow them itself.

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

#define SW_URL_MAX 512

typedef struct {
    httpcContext ctx;
    bool         open;

    // Guards `open` and the close, so that a thread calling swHttpCancel can
    // never be cancelling a context another thread has already torn down. It
    // is NOT held across a read: cancelling a read in progress is the entire
    // point of the call.
    LightLock    lock;

    // From the response headers, when the server sent them.
    int  metaint;              // icy-metaint: bytes of audio between metadata blocks
    int  bitrate;              // icy-br, in kbps; 0 if absent
    char server_name[96];      // icy-name: what the station calls itself
} SwHttp;

// Opens a GET for a radio stream: sends `Icy-MetaData: 1`, follows redirects,
// and reads the icy-* response headers into the struct.
//
// Certificate verification is DISABLED for streams. This is a deliberate,
// narrow decision: a large share of stations serve https with certificates the
// console's 2011-era trust store has never heard of, nothing secret is ever
// sent to a station, and the payload is audio that gets decoded and thrown
// away. The updater does the opposite - see update.c, where an unverified
// download would be an unverified executable.
bool swHttpOpenStream(SwHttp *h, const char *url);

// Reads up to `cap` bytes of body. Returns the byte count, 0 if the server has
// nothing ready this moment (not an error - a stream is paced by the clock),
// or -1 if the connection has failed or ended.
int swHttpRead(SwHttp *h, unsigned char *buf, size_t cap);

// Unblocks a read in progress from another thread, without closing anything.
//
// This exists because libctru exposes no socket timeout for httpc: a station
// that stops sending mid-song leaves swHttpRead blocked with no way out. The
// thread that wants playback to stop calls this; the reading thread's next
// return is a failure, and it tidies up and exits on its own. Safe to call on
// a handle that is already closed, and safe to call more than once.
void swHttpCancel(SwHttp *h);

// Cancels and closes. Safe to call on an already-closed handle. Call this from
// the thread that owns the handle - it is the owner that frees the context.
void swHttpClose(SwHttp *h);

// One-shot GET of a bounded resource, for the directory API. Writes at most
// `cap - 1` bytes and NUL-terminates. Returns the byte count, or -1.
// Verification is off here too: radio-browser is served over plain http by
// preference (see directory.c), and this exists to survive the https mirrors.
int swHttpGetText(const char *url, char *buf, size_t cap);
