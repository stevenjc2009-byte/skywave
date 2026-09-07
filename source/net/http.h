#pragma once

// HTTP for Skywave, on top of the app's own socket and TLS (see tcp.h).
//
// This used to sit on libctru's httpc. It no longer does, because httpc goes
// through the console's ssl:C module and is hard-capped at TLS 1.1 - which
// most streaming hosts and all four of GitHub's hosts refuse. Doing HTTP here
// means the app owns the parts httpc used to own: the request, the response
// head, redirects and chunked bodies. The framing of all of that lives in
// httpmsg.c, deliberately free of 3DS headers so it can be tested on a PC.
//
// Two very different jobs still share this file:
//
//   * One-shot GETs of a few tens of KB - the station directory. Ordinary.
//
//   * An endless GET that is never meant to finish - a radio stream. The
//     awkward one: it has no length, it must be interruptible from another
//     thread, and it carries metadata interleaved into the audio.

#include <stdbool.h>
#include <stddef.h>

#include "httpmsg.h"
#include "tcp.h"
#include "url.h"

// Bytes read from the socket in one go. Also the amount that can be held over
// between calls, which is whatever arrived in the same read as the end of the
// response head.
#define SW_HTTP_BUF 8192

// Why an open failed. One generic "could not connect" was a mistake: on a
// console there is no log to read afterwards, so the message on screen is the
// entire diagnostic record. These separate the cases that look identical to a
// user but have completely different answers - a station that is down, a
// station whose address is wrong, and a station whose certificate cannot be
// checked.
typedef enum {
    SW_HTTP_OK = 0,
    SW_HTTP_ERR_OPEN,       // the URL itself could not be understood
    SW_HTTP_ERR_CONNECT,    // no answer: DNS, refused, unreachable, timed out
    SW_HTTP_ERR_TLS,        // connected, but the TLS handshake failed
    SW_HTTP_ERR_CERT,       // handshake completed, certificate not trusted
    SW_HTTP_ERR_REDIRECT,   // a redirect with no Location, or a loop
    SW_HTTP_ERR_STATUS,     // the server answered, but not with 200
    SW_HTTP_ERR_HLS,        // answered 200, but with an HLS manifest, not audio
    SW_HTTP_ERR_CANCELLED,  // another thread asked for this to stop
} SwHttpResult;

// Tagged (not anonymous) so directory.h can forward-declare it and hand
// swDirRegisterPlayH a pointer without pulling this file - and tcp.h's
// mbedtls headers behind it - into every translation unit that only wants
// the directory's parsing half. See the forward declaration in directory.h.
typedef struct SwHttp {
    SwConn conn;
    bool   open;

    // Set on failure and kept, so the caller can say what went wrong rather
    // than that something did.
    SwHttpResult err;
    int          rc;           // the mbedtls error behind err, 0 if none
    int          status;       // the HTTP status, when one was received
    bool         tls;          // the hop that failed was https

    // There is no lock here, and that is deliberate.
    //
    // The old httpc version held a LightLock around open/close so a cancel from
    // the UI thread could not tear down a handle mid-flight. It was initialised
    // inside swHttpOpenStream, which left a genuine hole: swPlayerStop can call
    // swHttpCancel while the network thread is still inside the open, i.e.
    // before the lock exists. A lock that has to be created by the very call it
    // is protecting cannot protect it.
    //
    // Ownership rules replace it, and they are simpler to be sure of:
    //
    //   * One thread owns a handle. Only that thread calls open, read or close.
    //   * Any thread may call swHttpCancel. It sets one volatile flag and calls
    //     shutdown() - no state of this struct is written.
    //   * swConnClose clears `open` and takes the descriptor out of the struct
    //     before closing it, so a cancel racing a close hits fd -1 and fails
    //     harmlessly instead of shutting down a recycled descriptor.
    //
    // A cancel that arrives before the connection exists is not lost either:
    // swConnOpen preserves an already-set cancel flag across its own reset, and
    // polls it throughout the connect and the handshake.

    // From the response headers, when the server sent them.
    int  metaint;              // icy-metaint: audio bytes between metadata blocks
    int  bitrate;              // icy-br, in kbps; 0 if absent
    char server_name[96];      // icy-name: what the station calls itself

    // The full response head of the last successful open, kept so a caller can
    // ask for a header the struct does not have a field for - the updater wants
    // Content-Length, and one day something will want Content-Type.
    SwHttpHead head;

    // Body bytes already read from the socket - normally the tail of the read
    // that completed the head - waiting to be handed to the caller.
    unsigned char pend[SW_HTTP_BUF];
    size_t        pend_len;
    size_t        pend_off;

    bool      chunked;
    SwChunked chunk;

    // URL scratch. It lives in the struct rather than on the stack because a URL
    // is 2 KB here and the player's network thread gets a 16 KB stack: two of
    // these plus a parsed SwUrlParts is most of it, and a stack overflow on a
    // console is a silent hang rather than a crash with a message.
    char url[SW_URL_MAX];              // the hop currently being fetched
    char scratch[SW_URL_MAX + 512];    // request text, then the Location header
} SwHttp;

// Opens a GET for a radio stream: sends `Icy-MetaData: 1`, follows redirects,
// and reads the icy-* response headers into the struct.
//
// Certificate verification is DISABLED for streams. This is a deliberate,
// narrow decision: a fair share of stations serve https with certificates no
// bundle will vouch for, nothing secret is ever sent to a station, and the
// payload is audio that gets decoded and thrown away. The updater does the
// opposite - see update.c, where an unverified download is an unverified
// executable.
//
// Returns SW_HTTP_OK, or the reason it failed - which is also left in `h` for
// swHttpErrorText.
SwHttpResult swHttpOpenStream(SwHttp *h, const char *url);

// Writes a one-line explanation of the last failed open into `out`, carrying
// the real status code or error number. The number matters: it is the only
// thing distinguishing "this station is down today" from something permanent,
// and it is the only evidence a user can report.
void swHttpErrorText(const SwHttp *h, char *out, size_t cap);

// Reads up to `cap` bytes of body. Returns the byte count, 0 if the server has
// nothing ready this moment (not an error - a stream is paced by the clock),
// or -1 if the connection has failed or ended.
int swHttpRead(SwHttp *h, unsigned char *buf, size_t cap);

// Unblocks a read in progress from another thread, without closing anything.
// The reading thread's next return is a failure, and it tidies up and exits on
// its own. Safe on a closed handle, and safe to call more than once.
void swHttpCancel(SwHttp *h);

// Cancels and closes. Safe on an already-closed handle. Call this from the
// thread that owns the handle - it is the owner that frees the connection.
void swHttpClose(SwHttp *h);

// One-shot GET of a bounded resource, for the directory API. Writes at most
// `cap - 1` bytes and NUL-terminates. Returns the byte count, or -1.
// Verification is off here too: this exists to survive the https mirrors of a
// public, keyless directory.
int swHttpGetText(const char *url, char *buf, size_t cap);

// Same as swHttpGetText, but with caller-chosen bounds on the response head
// and a quiet body instead of the usual HEAD_TOTAL_MS / BODY_IDLE_MS.
//
// This exists because swHttpGetText allocates its own private SwHttp with no
// handle for anyone else to cancel - fine for a directory query, which is
// something the user is actively waiting on and will simply retry, but not
// for a call made synchronously off a thread that something else may be
// trying to shut down (see swDirRegisterPlay, the reason this exists). A
// bounded worst case there matters more than a generous one.
int swHttpGetTextBounded(const char *url, char *buf, size_t cap,
                         int head_total_ms, int body_idle_ms);

// Same as swHttpGetTextBounded, but fetches into a caller-owned handle `h`
// instead of a private one, so the caller can hold a pointer to `h` and call
// swHttpCancel(h) from another thread to unblock this fetch - the same
// mechanism swPlayerStop already uses on the stream handle. This is what
// swHttpGetTextBounded itself cannot offer, being the reason it was not
// enough for swDirRegisterPlay's ping (see swDirRegisterPlayH in
// directory.h, the caller this exists for).
//
// `h` must not be a stack local: sizeof(SwHttp) is 17,584 bytes here, and the
// intended caller is a network thread with a 16 KB stack. Give it a static or
// a zeroed heap block instead - `calloc`, or `malloc` followed by a memset to
// zero, matching exactly how the player's own stream handle (g.http) is
// declared. That zeroing matters on the first call only: `h->cancel` (via
// h->conn.cancelled) must start false, and after that this function's own
// close leaves it false for the next call - see the comment on swHttpClose in
// http.c for why the reset moved there instead of staying at the top of an
// open.
//
// Resets every other field itself (the same reset swHttpOpenStream relies on
// for the reused g.http), closes `h` before returning either way, and does
// not free it - ownership of `h`'s storage stays with the caller.
int swHttpGetTextBoundedH(SwHttp *h, const char *url, char *buf, size_t cap,
                          int head_total_ms, int body_idle_ms);

// ---- for the updater -----------------------------------------------------
//
// The update path wants what the stream path does not: certificate
// verification, and the Location of a redirect rather than the thing it points
// at. It shares the machinery above rather than hand-rolling a second client.

// Opens a GET with certificate verification ON, following redirects, and
// leaves the body ready to read with swHttpRead.
SwHttpResult swHttpOpenVerified(SwHttp *h, const char *url);

// Sends one request with verification ON and does NOT follow the redirect:
// copies the Location header into `location` and closes. This is how the
// updater reads the latest tag without downloading anything.
//
// Mallocs a private SwHttp that nothing outside this call can reach, so
// nothing outside this call can cancel it either - the same trade
// swHttpGetText makes, and fine for exactly the same reason: a caller with
// nothing else going on. sw_update_check's own blocking variant is that
// caller. See swHttpGetLocationH for the caller-owned version, and
// update.c's sw_update_check_h for why that one exists.
//
// Returns SW_HTTP_OK only when a redirect with a Location actually came back.
SwHttpResult swHttpGetLocation(const char *url, char *location, size_t cap);

// Same as swHttpGetLocation, but reads the redirect into a caller-owned
// handle `h` instead of a private one, so the caller can hold a pointer to
// `h` and call swHttpCancel(h) from another thread to unblock this call -
// the same mechanism swPlayerStop already uses on the stream handle, and
// swHttpGetTextBoundedH offers for swDirRegisterPlayH's ping. This is the
// piece that was missing for the app's startup update check: the check's
// own worst case is bounded (see update.c), but the getaddrinfo underneath
// it is not, and with no handle for anyone else to reach, quitting while a
// check was in flight had no way to shorten that wait. See update.c's
// sw_update_check_h and app.c's g.upd_http, the caller this exists for.
//
// `h` must not be a stack local: sizeof(SwHttp) is 17,584 bytes here, and
// the update-check thread has a 16 KB stack, same as every other caller of
// this pattern - see swHttpGetTextBoundedH above for the full reasoning and
// what "zeroed" needs to mean. app.c gives it a member of the app's global
// state, exactly as player.c does with g.ping.
//
// Resets every field it needs via open_common, closes `h` before returning
// either way, and does not free it - ownership of `h`'s storage stays with
// the caller.
SwHttpResult swHttpGetLocationH(SwHttp *h, const char *url, char *location,
                                size_t cap);

// The value of a response header from the last opened handle.
bool swHttpHeader(const SwHttp *h, const char *name, char *out, size_t cap);
