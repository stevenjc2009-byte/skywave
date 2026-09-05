#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../version.h"

// radio-browser.info asks apps to identify themselves, and GitHub refuses
// requests that do not. One string satisfies both.
#define USER_AGENT "Skywave/" SKYWAVE_VERSION " (Nintendo 3DS)"

#define MAX_HOPS 6

static bool is_redirect(u32 status)
{
    return (status >= 301 && status <= 303) || (status >= 307 && status <= 308);
}

// Opens one hop and reads the status line. On failure nothing is left open.
static bool begin(httpcContext *ctx, const char *url, u32 *status)
{
    if (R_FAILED(httpcOpenContext(ctx, HTTPC_METHOD_GET, url, 1))) return false;

    // See the header for why verification is off on this path.
    httpcSetSSLOpt(ctx, SSLCOPT_DisableVerify);
    httpcSetKeepAlive(ctx, HTTPC_KEEPALIVE_ENABLED);
    httpcAddRequestHeaderField(ctx, "User-Agent", USER_AGENT);
    httpcAddRequestHeaderField(ctx, "Connection", "Keep-Alive");
    // Asking for metadata is harmless on a server that does not do it, and it
    // is the only way to get now-playing text out of an Icecast stream.
    httpcAddRequestHeaderField(ctx, "Icy-MetaData", "1");

    if (R_FAILED(httpcBeginRequest(ctx)) ||
        R_FAILED(httpcGetResponseStatusCode(ctx, status))) {
        httpcCloseContext(ctx);
        return false;
    }
    return true;
}

// Follows redirects by hand. Leaves the context open on whichever response
// finally carried a body.
static bool get_following(httpcContext *ctx, const char *url, u32 *status)
{
    char next[SW_URL_MAX];
    snprintf(next, sizeof(next), "%s", url);

    for (int hop = 0; hop < MAX_HOPS; hop++) {
        if (!begin(ctx, next, status)) return false;
        if (!is_redirect(*status)) return true;

        bool got = R_SUCCEEDED(httpcGetResponseHeader(ctx, "Location",
                                                      next, sizeof(next)));
        httpcCloseContext(ctx);
        if (!got) return false;
    }
    return false;  // a redirect loop
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

bool swHttpOpenStream(SwHttp *h, const char *url)
{
    memset(h, 0, sizeof(*h));
    LightLock_Init(&h->lock);

    u32 status = 0;
    if (!get_following(&h->ctx, url, &status)) return false;

    // Icecast answers a stream request with 200. Anything else - a 404 on a
    // dead mount, a 302 loop, a 503 when the station is over its listener cap -
    // is not something to start decoding.
    if (status != 200) {
        httpcCloseContext(&h->ctx);
        return false;
    }

    h->open    = true;
    h->metaint = header_int(&h->ctx, "icy-metaint", 0);
    h->bitrate = header_int(&h->ctx, "icy-br", 0);
    httpcGetResponseHeader(&h->ctx, "icy-name", h->server_name, sizeof(h->server_name));

    return true;
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
    if (!get_following(&ctx, url, &status)) return -1;

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
