#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "url.h"
#include "../version.h"

// radio-browser.info asks apps to identify themselves, and GitHub refuses
// requests that do not. One string satisfies both.
#define USER_AGENT "Skywave/" SKYWAVE_VERSION " (Nintendo 3DS)"

#define MAX_HOPS 6

static bool is_redirect(u32 status)
{
    return (status >= 301 && status <= 303) || (status >= 307 && status <= 308);
}

// Carries the reason a hop failed back out of begin(). Kept separate from
// SwHttp because swHttpGetText has no SwHttp to write into.
typedef struct {
    SwHttpResult err;
    Result       rc;
    u32          status;
    bool         tls;
} Attempt;

// Opens one hop and reads the status line. On failure nothing is left open.
static bool begin(httpcContext *ctx, const char *url, u32 *status, Attempt *a)
{
    // Recorded before anything can go wrong, because which scheme was being
    // attempted is most of the diagnosis when it does.
    a->tls = strncmp(url, "https:", 6) == 0;

    Result rc = httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 1);
    if (R_FAILED(rc)) {
        a->err = SW_HTTP_ERR_OPEN;
        a->rc  = rc;
        return false;
    }

    // See the header for why verification is off on this path.
    httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(ctx, "User-Agent", USER_AGENT);
    httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");
    // Asking for metadata is harmless on a server that does not do it, and it
    // is the only way to get now-playing text out of an Icecast stream.
    httpcAddRequestHeaderField(ctx, "Icy-MetaData", "1");

    // These two are one case as far as the caller is concerned: the request
    // went out and no usable answer came back. On an https URL this is where a
    // TLS handshake the console cannot complete lands.
    rc = httpcBeginRequest(ctx);
    if (R_SUCCEEDED(rc)) rc = httpcGetResponseStatusCode(ctx, status);
    if (R_FAILED(rc)) {
        a->err = SW_HTTP_ERR_CONNECT;
        a->rc  = rc;
        httpcCloseContext(ctx);
        return false;
    }

    // A status outside 100..599 is not an HTTP status, so nothing answered -
    // whatever the return code said. Measured under Azahar 2126.0 on
    // 2026-09-05: a TLS handshake the server refuses outright comes back as
    // success, rc 0x00000000, status 0xFFFFFFFF. Without this the caller reads
    // that as "the station answered, just not with 200" and never retries, and
    // the whole downgrade path below is dead code. Treating it as a connect
    // failure is not emulator-specific tuning: an HTTP response code is defined
    // to be three digits, so a value that is not one means there was no
    // response to read.
    if (*status < 100 || *status > 599) {
        a->err = SW_HTTP_ERR_CONNECT;
        a->rc  = rc;
        httpcCloseContext(ctx);
        return false;
    }
    return true;
}

// Opens one hop, and if an https hop cannot be connected at all, tries the same
// address as plain http before giving up. `url` is rewritten to whatever
// actually worked, so redirects continue from the right place.
//
// Why this exists. The console's TLS is not merely old, it is fixed: ssl:C tops
// out at TLS 1.1 with RSA key exchange and CBC ciphers, and there is no libctru
// option that raises it - SSLCOPT_DisableVerify only skips certificate checks,
// which is a different failure entirely. Measured against the default station
// list on 2026-09-05: of 23 https stations, 19 refuse a handshake in that shape,
// and 8 of those 19 serve the identical mount over plain http, with several more
// redirecting to an http address once asked. Retrying is the only thing that
// recovers them short of linking a software TLS stack.
//
// Nothing is given up by the downgrade. Verification is already off on this path
// (see the header), the request carries no credentials, and the payload is
// public audio that is decoded and thrown away.
static bool begin_or_plain(httpcContext *ctx, char *url, size_t cap,
                           u32 *status, Attempt *a)
{
    if (begin(ctx, url, status, a)) return true;

    // Only a connect-stage failure is worth retrying. A 404 or a refused URL
    // will fail the same way on either scheme.
    if (a->err != SW_HTTP_ERR_CONNECT) return false;

    char plain[SW_URL_MAX];
    if (!swUrlToPlainHttp(url, plain, sizeof(plain))) return false;

    if (!begin(ctx, plain, status, a)) {
        // begin() has just overwritten `tls` with the plain attempt's scheme.
        // Put it back, or the error would blame the station's http port for a
        // station that was only ever offered over https.
        a->tls = true;
        return false;
    }

    snprintf(url, cap, "%s", plain);
    return true;
}

// Follows redirects by hand. Leaves the context open on whichever response
// finally carried a body.
static bool get_following(httpcContext *ctx, const char *url, u32 *status,
                          Attempt *a)
{
    char next[SW_URL_MAX];
    snprintf(next, sizeof(next), "%s", url);

    memset(a, 0, sizeof(*a));

    for (int hop = 0; hop < MAX_HOPS; hop++) {
        if (!begin_or_plain(ctx, next, sizeof(next), status, a)) return false;
        if (!is_redirect(*status)) return true;

        bool got = R_SUCCEEDED(httpcGetResponseHeader(ctx, "Location",
                                                      next, sizeof(next)));
        httpcCloseContext(ctx);
        if (!got) {
            a->err    = SW_HTTP_ERR_REDIRECT;
            a->status = *status;
            return false;
        }
    }

    a->err = SW_HTTP_ERR_REDIRECT;   // ran out of hops
    return false;
}

// Reads an integer response header, returning `fallback` if it is absent or
// not a number.
static int header_int(httpcContext *ctx, const char *name, int fallback)
{
    char v[32] = {0};
    if (R_FAILED(httpcGetResponseHeader(ctx, (char *)name, v, sizeof(v)))) return fallback;

    int out = 0;
    const char *p = v;
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return fallback;
    while (*p >= '0' && *p <= '9') out = out * 10 + (*p++ - '0');
    return out;
}

SwHttpResult swHttpOpenStream(SwHttp *h, const char *url)
{
    memset(h, 0, sizeof(*h));
    LightLock_Init(&h->lock);

    u32 status = 0;
    Attempt a;
    if (!get_following(&h->ctx, url, &status, &a)) {
        h->err    = a.err;
        h->rc     = a.rc;
        h->status = a.status;
        h->tls    = a.tls;
        return h->err;
    }

    // Icecast answers a stream request with 200. Anything else - a 404 on a
    // dead mount, a 302 loop, a 503 when the station is over its listener cap -
    // is not something to start decoding.
    if (status != 200) {
        httpcCloseContext(&h->ctx);
        h->err    = SW_HTTP_ERR_STATUS;
        h->status = status;
        h->tls    = a.tls;
        return h->err;
    }

    h->open    = true;
    h->metaint = header_int(&h->ctx, "icy-metaint", 0);
    h->bitrate = header_int(&h->ctx, "icy-br", 0);
    httpcGetResponseHeader(&h->ctx, "icy-name", h->server_name, sizeof(h->server_name));

    return SW_HTTP_OK;
}

void swHttpErrorText(const SwHttp *h, char *out, size_t cap)
{
    if (cap == 0) return;

    switch (h->err) {
        case SW_HTTP_OK:
            out[0] = 0;
            return;

        case SW_HTTP_ERR_OPEN:
            snprintf(out, cap, "Bad stream address. (0x%08lX)",
                     (unsigned long)h->rc);
            return;

        case SW_HTTP_ERR_CONNECT:
            // Worth splitting: an https failure here is very often the console
            // and the station failing to agree on TLS, which is permanent for
            // that station. A plain http failure is usually the station being
            // down, which is not.
            if (h->tls)
                snprintf(out, cap, "HTTPS failed - the 3DS may be too old for "
                                   "this station. (0x%08lX)",
                         (unsigned long)h->rc);
            else
                snprintf(out, cap, "Could not reach this station. (0x%08lX)",
                         (unsigned long)h->rc);
            return;

        case SW_HTTP_ERR_REDIRECT:
            snprintf(out, cap, "This station's address redirects in circles.");
            return;

        case SW_HTTP_ERR_STATUS:
            snprintf(out, cap, "Station answered %lu, not audio.",
                     (unsigned long)h->status);
            return;
    }

    snprintf(out, cap, "Could not connect to this station.");
}

int swHttpRead(SwHttp *h, unsigned char *buf, size_t cap)
{
    if (!h->open) return -1;

    u32 got = 0;
    Result rc = httpcDownloadData(&h->ctx, buf, (u32)cap, &got);

    // DOWNLOADPENDING is the normal state of affairs on a stream: it means the
    // server has more to send, which for live radio is true forever.
    if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) return (int)got;
    if (R_FAILED(rc)) return -1;

    // A clean end. For a radio stream this means the station dropped us.
    return got > 0 ? (int)got : -1;
}

void swHttpCancel(SwHttp *h)
{
    LightLock_Lock(&h->lock);
    if (h->open) httpcCancelConnection(&h->ctx);
    LightLock_Unlock(&h->lock);
}

void swHttpClose(SwHttp *h)
{
    LightLock_Lock(&h->lock);
    if (!h->open) { LightLock_Unlock(&h->lock); return; }

    // The order matters. httpcCloseContext waits for a download to complete,
    // and a live stream never completes, so closing without cancelling first
    // hangs the thread that called it - which is the network thread.
    httpcCancelConnection(&h->ctx);
    httpcCloseContext(&h->ctx);
    h->open = false;
    LightLock_Unlock(&h->lock);
}

int swHttpGetText(const char *url, char *buf, size_t cap)
{
    if (cap == 0) return -1;

    httpcContext ctx;
    u32 status = 0;
    Attempt a;
    if (!get_following(&ctx, url, &status, &a)) return -1;

    if (status != 200) {
        httpcCloseContext(&ctx);
        return -1;
    }

    size_t total = 0;
    for (;;) {
        u32 got = 0;
        size_t room = cap - 1 - total;
        if (room == 0) break;   // caller's buffer is full; take what we have

        Result rc = httpcDownloadData(&ctx, (u8 *)buf + total, (u32)room, &got);
        total += got;

        if (rc == (Result)HTTPC_RESULTCODE_DOWNLOADPENDING) continue;
        if (R_FAILED(rc)) {
            httpcCloseContext(&ctx);
            return -1;
        }
        break;
    }

    buf[total] = 0;
    httpcCloseContext(&ctx);
    return (int)total;
}
