#include "http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../store/diag.h"
#include "../version.h"

// radio-browser.info asks apps to identify themselves, and GitHub refuses
// requests that do not. One string satisfies both.
#define USER_AGENT "Skywave/" SKYWAVE_VERSION " (Nintendo 3DS)"

// A station's vanity domain to its CDN to a regional edge is three hops, and
// GitHub's release download is two. Six leaves headroom without letting a
// redirect loop spin until the user gives up.
#define MAX_HOPS 6

// How long a stream read waits before saying "nothing yet". Short, because the
// caller is a decode loop with other work to do and 0 is not a failure.
#define STREAM_READ_MS 200

// The response head is the one part of the exchange a server has no reason to
// be slow about. Waiting longer than this means something is wrong.
#define HEAD_SLICE_MS 500
#define HEAD_TOTAL_MS 10000

// For a one-shot GET: how long the body may go quiet before it counts as over.
// Only reached when a server neither closes the connection nor sent a length.
#define BODY_IDLE_MS 10000

static bool is_redirect(int status)
{
    return (status >= 301 && status <= 303) || (status >= 307 && status <= 308);
}

static SwHttpResult from_conn(SwConnResult r)
{
    switch (r) {
        case SW_CONN_ERR_TLS:       return SW_HTTP_ERR_TLS;
        case SW_CONN_ERR_CERT:      return SW_HTTP_ERR_CERT;
        case SW_CONN_ERR_CANCELLED: return SW_HTTP_ERR_CANCELLED;
        case SW_CONN_ERR_RESOLVE:
        case SW_CONN_ERR_CONNECT:
        default:                    return SW_HTTP_ERR_CONNECT;
    }
}

static bool send_request(SwHttp *h, const SwUrlParts *u, bool icy)
{
    // Credentials that came in the URL become a header rather than staying in
    // the request line. Directory entries really do carry them, and a server
    // sent `GET http://user:pass@host/path` either answers 400 or, worse, treats
    // the whole thing as a path and serves the wrong mount.
    //
    // What goes in `auth` is the bare base64, with no scheme token, because
    // swHttpBuildGet writes "Authorization: Basic %s" itself. This line used to
    // add a second "Basic " of its own, so the header on the wire read
    // `Authorization: Basic Basic dXNlcjpwYXNz` and every credentialed station
    // answered 401. swUrlBasicAuth is where that rule now lives, so it can be
    // held to it by a host test - nothing in this file is covered by one.
    char auth[SW_URL_AUTH_MAX * 2];
    auth[0] = 0;
    if (u->userinfo[0] && !swUrlBasicAuth(u->userinfo, auth, sizeof(auth)))
        return false;

    int n = swHttpBuildGet(h->scratch, sizeof(h->scratch),
                           u->host, u->port, u->path,
                           USER_AGENT, auth[0] ? auth : NULL, icy, NULL);
    if (n < 0) return false;

    return swConnWrite(&h->conn, h->scratch, (size_t)n);
}

// Reads until the response head is complete, keeping whatever body bytes arrived
// in the same read. On success `pend_off` points at the first body byte.
//
// `head_total_ms` is a parameter rather than always HEAD_TOTAL_MS so that
// swHttpGetTextBounded can give a fire-and-forget caller a shorter leash -
// see the comment on that function.
static SwHttpResult read_head(SwHttp *h, int head_total_ms)
{
    h->pend_len = 0;
    h->pend_off = 0;

    int waited = 0;
    for (;;) {
        size_t body_off = 0;
        SwHeadState st = swHttpHeadParse((const char *)h->pend, h->pend_len,
                                         &h->head, &body_off);
        if (st == SW_HEAD_OK) {
            h->pend_off = body_off;
            h->status   = h->head.status;
            return SW_HTTP_OK;
        }
        if (st == SW_HEAD_BAD) return SW_HTTP_ERR_STATUS;

        // A whole buffer with no blank line in it means this is not an HTTP
        // server, or the head is absurd. Reading more will not help either way.
        if (h->pend_len >= sizeof(h->pend)) return SW_HTTP_ERR_STATUS;

        int n = swConnRead(&h->conn, h->pend + h->pend_len,
                           sizeof(h->pend) - h->pend_len, HEAD_SLICE_MS);
        if (n < 0)
            return h->conn.cancelled ? SW_HTTP_ERR_CANCELLED : SW_HTTP_ERR_CONNECT;

        if (n == 0) {
            waited += HEAD_SLICE_MS;
            if (waited >= head_total_ms) return SW_HTTP_ERR_CONNECT;
            continue;
        }
        h->pend_len += (size_t)n;
    }
}

// One request, following redirects unless `follow` is false - in which case a
// redirect is itself the answer and its Location is copied to `location`.
//
// The address is read from, and rewritten in, h->url.
static SwHttpResult open_url(SwHttp *h, bool verify, bool icy, bool follow,
                             char *location, size_t loc_cap, int head_total_ms)
{
    // Credentials survive a redirect, but only back to the host that issued
    // them. A station whose address carries `user:pass@` very often answers the
    // first request with a mount redirect, and swUrlResolve rebuilds that
    // Location from host, port and path alone - userinfo is not part of what it
    // writes, so without this the second hop arrives unauthenticated and the
    // 401 the credentials existed to prevent happens one hop later instead.
    //
    // Keyed on the host and nothing else: a station redirecting to a CDN is a
    // different host and must never be handed the origin's password.
    char cred[SW_URL_AUTH_MAX];
    char cred_host[SW_URL_HOST_MAX];
    cred[0] = 0;
    cred_host[0] = 0;

    for (int hop = 0; hop < MAX_HOPS; hop++) {
        SwUrlParts u;
        if (!swUrlSplit(h->url, &u)) return SW_HTTP_ERR_OPEN;

        if (u.userinfo[0]) {
            // This hop carried its own credentials; they become the ones a
            // later same-host hop may inherit.
            snprintf(cred, sizeof(cred), "%s", u.userinfo);
            snprintf(cred_host, sizeof(cred_host), "%s", u.host);
        } else if (cred[0] && swUrlHostEq(u.host, cred_host)) {
            snprintf(u.userinfo, sizeof(u.userinfo), "%s", cred);
        }

        h->tls = u.tls;

        // A verified fetch is not allowed to be talked out of TLS by a redirect.
        //
        // The fallback below is gated on !verify and is genuinely unreachable
        // here, but that gate only covers a handshake that fails. A hop
        // answering "Location: http://..." never goes near it: the next time
        // round this loop u.tls is simply false, swConnOpen returns before it
        // looks at `verify` at all, and the download arrives over plain TCP
        // having been verified by nothing. Since what the updater downloads is
        // an executable the console then installs, that path has to be closed
        // explicitly rather than by implication.
        if (verify && !u.tls) return SW_HTTP_ERR_CERT;

        SwConnResult cr = swConnOpen(&h->conn, u.host, u.port, u.tls, verify);

        // Falling back to plain http when TLS will not come together.
        //
        // This is for streams only, and `!verify` is what keeps it there. The
        // updater must never be silently downgraded: an unverified download of
        // an executable is the one case in this app where the content matters.
        // For a station it is the difference between music and an error message,
        // nothing secret travels either way, and the payload is public audio
        // that gets decoded and thrown away.
        //
        // Far rarer now than it was under httpc - the app's own TLS reaches
        // hosts ssl:C could not - but a station with an expired certificate or a
        // broken chain still exists, and this is what recovers it.
        if ((cr == SW_CONN_ERR_TLS || cr == SW_CONN_ERR_CERT) && u.tls && !verify) {
            u.tls = false;
            if (strcmp(u.port, "443") == 0) snprintf(u.port, sizeof(u.port), "80");
            h->tls = false;
            cr = swConnOpen(&h->conn, u.host, u.port, false, false);
        }

        if (cr != SW_CONN_OK) {
            h->rc = swConnLastError(&h->conn);
            return from_conn(cr);
        }

        if (!send_request(h, &u, icy)) {
            bool cancelled = h->conn.cancelled;
            swConnClose(&h->conn);
            return cancelled ? SW_HTTP_ERR_CANCELLED : SW_HTTP_ERR_CONNECT;
        }

        SwHttpResult r = read_head(h, head_total_ms);
        if (r != SW_HTTP_OK) {
            h->rc = swConnLastError(&h->conn);
            swConnClose(&h->conn);
            return r;
        }

        if (is_redirect(h->status)) {
            if (!swHttpHeadValue(&h->head, "Location", h->scratch, sizeof(h->scratch))) {
                swConnClose(&h->conn);
                return SW_HTTP_ERR_REDIRECT;   // a redirect pointing nowhere
            }

            if (!follow) {
                swConnClose(&h->conn);
                if (!location || strlen(h->scratch) + 1 > loc_cap)
                    return SW_HTTP_ERR_REDIRECT;
                snprintf(location, loc_cap, "%s", h->scratch);
                return SW_HTTP_OK;
            }

            // Resolved against the hop that produced it, because Location is
            // often a bare path - Icecast answers a mount redirect that way.
            // `u` is a copy, so writing the result over h->url is safe.
            if (!swUrlResolve(&u, h->scratch, h->url, sizeof(h->url))) {
                swConnClose(&h->conn);
                return SW_HTTP_ERR_REDIRECT;
            }
            swConnClose(&h->conn);
            continue;
        }

        // Icecast answers a stream request with 200. 206 is allowed because a
        // few servers answer a plain GET with partial content, which for an
        // endless stream is the same thing. Anything else - a 404 on a dead
        // mount, a 503 when the station is over its listener cap - is not
        // something to start decoding.
        if (h->status != 200 && h->status != 206) {
            swConnClose(&h->conn);
            return SW_HTTP_ERR_STATUS;
        }

        // A station can answer 200, even with a codec the directory calls
        // playable, and still not be an audio stream at all: HLS serves a text
        // manifest of segment URLs from the same address a station's stream
        // would use. MEASURED 2026-09-07 against two live BBC Radio 1 entries
        // (codec "AAC+", hls=0 already in the query - see codec_playable()'s own
        // comment in directory_parse.c on why that flag is not trustworthy):
        // both answered 200, Content-Type "application/x-mpegurl", body starting
        // "#EXTM3U\n#EXT-X-VERSION:3\n...". Handing that to mpg123 or Helix
        // produces noise or silence with no way for either decoder to say why -
        // this is caught here instead, where the real reason is still known.
        //
        // Gated on `icy`, which is true only for swHttpOpenStream: the updater
        // and the directory's own JSON fetch share this function and must not be
        // touched by a check that exists for one failure mode of one caller.
        if (icy && swHttpIsHlsManifest(&h->head, (const char *)h->pend + h->pend_off,
                                       h->pend_len - h->pend_off)) {
            swConnClose(&h->conn);
            return SW_HTTP_ERR_HLS;
        }

        h->chunked = swHttpHeadChunked(&h->head);
        swChunkedInit(&h->chunk);

        h->metaint = (int)swHttpHeadInt(&h->head, "icy-metaint", 0);
        h->bitrate = (int)swHttpHeadInt(&h->head, "icy-br", 0);
        if (!swHttpHeadValue(&h->head, "icy-name", h->server_name, sizeof(h->server_name)))
            h->server_name[0] = 0;

        h->open = true;
        return SW_HTTP_OK;
    }

    return SW_HTTP_ERR_REDIRECT;   // went round MAX_HOPS times and never arrived
}

static SwHttpResult open_common(SwHttp *h, const char *url, bool verify, bool icy,
                                bool follow, char *location, size_t loc_cap,
                                int head_total_ms)
{
    if (!h || !url) return SW_HTTP_ERR_OPEN;

    h->open           = false;
    h->err            = SW_HTTP_OK;
    h->rc             = 0;
    h->status         = 0;
    h->tls            = false;
    h->metaint        = 0;
    h->bitrate        = 0;
    h->server_name[0] = 0;
    h->pend_len       = 0;
    h->pend_off       = 0;
    h->chunked        = false;
    h->head.status    = 0;
    h->head.icy       = false;
    h->head.raw_len   = 0;
    h->head.raw[0]    = 0;
    swChunkedInit(&h->chunk);

    // h->conn.cancelled is deliberately NOT cleared here. It used to be, but
    // "here" is the start of a fresh open, running on whatever thread is about
    // to own it - and for a reused handle like g.http, that thread was just
    // created. A cancel meant for the very connection about to be opened can
    // land in the gap between that thread starting and this line running, and
    // clearing it here would erase it: the connection would then run to its
    // full timeout budget instead of stopping, exactly the hang this comment
    // once claimed could not happen. See swHttpClose for where the clear moved
    // to, and why that point is actually safe.
    //
    // What that means for a fresh handle: cancelled must already be false when
    // this is first reached, which a zero-initialised SwHttp (static storage,
    // or calloc'd / memset heap) gives for free. Every caller here does one of
    // those - see swHttpGetText, swHttpGetLocation and player.c's g.http.

    if (strlen(url) + 1 > sizeof(h->url)) {
        h->err = SW_HTTP_ERR_OPEN;
        return h->err;
    }
    snprintf(h->url, sizeof(h->url), "%s", url);

    SwHttpResult r = open_url(h, verify, icy, follow, location, loc_cap, head_total_ms);
    if (r != SW_HTTP_OK) h->err = r;
    return r;
}

SwHttpResult swHttpOpenStream(SwHttp *h, const char *url)
{
    return open_common(h, url, /*verify*/ false, /*icy*/ true,
                       /*follow*/ true, NULL, 0, HEAD_TOTAL_MS);
}

SwHttpResult swHttpOpenVerified(SwHttp *h, const char *url)
{
    return open_common(h, url, /*verify*/ true, /*icy*/ false,
                       /*follow*/ true, NULL, 0, HEAD_TOTAL_MS);
}

SwHttpResult swHttpGetLocationH(SwHttp *h, const char *url, char *location, size_t cap)
{
    if (!h || !url || !location || cap == 0) return SW_HTTP_ERR_OPEN;

    SwHttpResult r = open_common(h, url, /*verify*/ true, /*icy*/ false,
                                 /*follow*/ false, location, cap, HEAD_TOTAL_MS);

    // With follow off, open_url returns OK for a redirect. Anything else that
    // came back - a 200, a 404 - is a server answering a question that was not
    // the one being asked, and is not a location.
    if (r == SW_HTTP_OK && !is_redirect(h->status)) r = SW_HTTP_ERR_REDIRECT;

    swHttpClose(h);
    return r;
}

SwHttpResult swHttpGetLocation(const char *url, char *location, size_t cap)
{
    if (!url || !location || cap == 0) return SW_HTTP_ERR_OPEN;

    // On the heap: SwHttp is about 17 KB and this can be called from a thread
    // with a 16 KB stack. Private to this call - see swHttpGetLocationH in
    // http.h for why that is precisely what makes it uncancellable, and who
    // needs the caller-owned version instead.
    SwHttp *h = (SwHttp *)malloc(sizeof(SwHttp));
    if (!h) return SW_HTTP_ERR_OPEN;
    memset(h, 0, sizeof(*h));

    SwHttpResult r = swHttpGetLocationH(h, url, location, cap);

    free(h);
    return r;
}

void swHttpErrorText(const SwHttp *h, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    if (!h) { snprintf(out, cap, "Could not connect to this station."); return; }

    switch (h->err) {
        case SW_HTTP_OK:
            out[0] = 0;
            return;

        case SW_HTTP_ERR_OPEN:
            snprintf(out, cap, "Bad stream address.");
            return;

        case SW_HTTP_ERR_CONNECT:
            snprintf(out, cap, "Could not reach this station. (%d)", h->rc);
            return;

        case SW_HTTP_ERR_TLS:
            // Now genuinely the station's end. The app's own TLS negotiates 1.2
            // with ECDHE, so this is no longer the console being too old - it is
            // a server that offered nothing usable, or hung up mid-handshake.
            snprintf(out, cap, "Secure connection failed. (%d)", h->rc);
            return;

        case SW_HTTP_ERR_CERT: {
            // These two look identical on screen otherwise and have completely
            // different answers: one is the station's problem, the other is a
            // missing file the user can put on their SD card.
            const char *why = swNetHaveTrustStore()
                                ? "This station's certificate was rejected."
                                : "No certificate bundle installed.";
            snprintf(out, cap, "%s (%d)", why, h->rc);
            return;
        }

        case SW_HTTP_ERR_REDIRECT:
            snprintf(out, cap, "This station's address redirects in circles.");
            return;

        case SW_HTTP_ERR_STATUS:
            // A Shoutcast v1 server answers "ICY <n> <reason>" instead of
            // "HTTP/1.x <n> <reason>", and its numbers are its OWN vocabulary -
            // they are not HTTP status codes that happen to share a transport.
            // swHttpHeadParse already accepts and flags that status line
            // (head.icy), and this is the one place the distinction changes
            // what the user should be told, because the overlap is worst on
            // exactly the two codes that get reported:
            //
            //   ICY 401 is "service unavailable" - the mount is not currently
            //   broadcasting. HTTP 401 is "unauthorized". Sending a listener to
            //   look for a password because a station is off the air at 3am is
            //   the most misleading thing this function could do, and it was
            //   doing it: every ICY response fell through to the HTTP table
            //   below and 401 there says "needs a login".
            //
            //   ICY 400 is the server refusing the listener, most often because
            //   it is at its listener cap - which is the same situation HTTP
            //   stations report as 503, not the malformed-request meaning HTTP
            //   400 has.
            //
            // PROVENANCE: reasoned from Shoutcast v1's documented status
            // vocabulary, not measured. Nothing here has been tested against a
            // live Shoutcast v1 server - there is no way to do that from this
            // machine - so the numbers stay on screen in every branch and the
            // wording avoids naming a cause it cannot prove. If a station ever
            // contradicts this, the code in the message is what makes that
            // visible rather than hidden.
            if (h->head.icy) {
                switch (h->status) {
                    case 400:
                        snprintf(out, cap, "This station is full. (ICY 400)");
                        return;
                    case 401:
                        snprintf(out, cap, "This station is not broadcasting right now. (ICY 401)");
                        return;
                    case 404:
                        snprintf(out, cap, "This station's stream is gone. (ICY 404)");
                        return;
                }
                snprintf(out, cap, "Station answered ICY %d, not audio.", h->status);
                return;
            }

            // The bare number told the user nothing they could act on, and the
            // four codes below are the ones actually seen in the wild. They
            // have completely different answers - one is a station that wants
            // a login the directory never gave us, one is a mount that has
            // been taken down, and one is the station disliking a request we
            // built. The code stays on the end so a bug report is still
            // precise.
            switch (h->status) {
                case 400:
                    snprintf(out, cap, "This station rejected the request. (400)");
                    return;
                case 401:
                case 403:
                    snprintf(out, cap, "This station needs a login. (%d)", h->status);
                    return;
                case 404:
                    snprintf(out, cap, "This station's stream is gone. (404)");
                    return;
                case 503:
                    snprintf(out, cap, "This station is full or offline. (503)");
                    return;
            }
            snprintf(out, cap, "Station answered %d, not audio.", h->status);
            return;

        case SW_HTTP_ERR_HLS:
            snprintf(out, cap, "This station's stream is HLS, which this app cannot play.");
            return;

        case SW_HTTP_ERR_CANCELLED:
            snprintf(out, cap, "Stopped.");
            return;
    }

    snprintf(out, cap, "Could not connect to this station.");
}

int swHttpRead(SwHttp *h, unsigned char *buf, size_t cap)
{
    if (!h || !h->open || !buf || cap == 0) return -1;

    for (;;) {
        if (h->pend_off < h->pend_len) {
            if (!h->chunked) {
                size_t n = h->pend_len - h->pend_off;
                if (n > cap) n = cap;
                memcpy(buf, h->pend + h->pend_off, n);
                h->pend_off += n;
                return (int)n;
            }

            size_t consumed = 0;
            int got = swChunkedFeed(&h->chunk, h->pend + h->pend_off,
                                    h->pend_len - h->pend_off,
                                    &consumed, buf, cap);
            h->pend_off += consumed;
            if (got < 0) return -1;
            if (got > 0) return got;
            if (h->chunk.done) return -1;      // the body ended cleanly
            if (consumed == 0) return -1;      // no progress: malformed framing
            // Only chunk framing was consumed this time; go and read more.
        }

        h->pend_len = 0;
        h->pend_off = 0;

        int n = swConnRead(&h->conn, h->pend, sizeof(h->pend), STREAM_READ_MS);
        if (n < 0) return -1;
        if (n == 0) return 0;
        h->pend_len = (size_t)n;
    }
}

void swHttpCancel(SwHttp *h)
{
    // Lock-free and safe from any thread, including while the owning thread is
    // still inside an open. See the note on SwHttp in http.h for why there is no
    // lock here any more.
    if (h) swConnCancel(&h->conn);
}

void swHttpClose(SwHttp *h)
{
    if (!h) return;

    // Cancel first, exactly as the httpc version did and for the same reason: a
    // close that waits for the transfer to finish would wait forever on a live
    // stream, and the thread it would hang is the network thread.
    swConnCancel(&h->conn);
    swConnClose(&h->conn);

    h->open     = false;
    h->pend_len = 0;
    h->pend_off = 0;

    // Cleared here, not at the top of the next open_common. This is the point
    // where the thread that owns `h` is genuinely finished with the connection
    // this flag refers to - for a stream, the last thing net_main does before
    // the thread swPlayerStop's threadJoin is waiting on actually exits. A
    // cancel that lands after this line is, by construction, about whatever
    // opens `h` next - there is nothing else left for it to mean - so nothing
    // legitimate is lost, unlike clearing it before the next open ever ran
    // (see the comment in open_common this replaced).
    h->conn.cancelled = false;
}

bool swHttpHeader(const SwHttp *h, const char *name, char *out, size_t cap)
{
    if (!h) return false;
    return swHttpHeadValue(&h->head, name, out, cap);
}

// Shared body of swHttpGetText, swHttpGetTextBounded and swHttpGetTextBoundedH
// - see the comment on the latter two in http.h for why the timeouts are a
// parameter rather than always HEAD_TOTAL_MS / BODY_IDLE_MS, and why `h` is a
// parameter rather than always a private handle.
//
// Does not touch `h` before open_common does - no memset here. open_common
// resets every field it needs (see its own comment on h->conn.cancelled in
// particular), the same way swHttpOpenStream relies on it for the reused
// g.http; a handle-owning caller re-clearing cancelled itself right before
// this ran would reopen exactly the race that comment describes.
static int get_text_into(SwHttp *h, const char *url, char *buf, size_t cap,
                         int head_total_ms, int body_idle_ms)
{
    if (!h || !url || !buf || cap < 2) return -1;

    int out = -1;

    if (open_common(h, url, /*verify*/ false, /*icy*/ false,
                    /*follow*/ true, NULL, 0, head_total_ms) == SW_HTTP_OK) {
        size_t got  = 0;
        int    idle = 0;

        while (got + 1 < cap) {
            int n = swHttpRead(h, (unsigned char *)buf + got, cap - 1 - got);
            if (n < 0) break;              // the body ended, which is how this ends
            if (n == 0) {
                idle += STREAM_READ_MS;
                if (idle >= body_idle_ms) break;
                continue;
            }
            idle = 0;
            got += (size_t)n;
        }

        buf[got] = 0;
        // Zero bytes is reported as failure rather than as an empty document:
        // the only caller parses JSON, and "" would surface as a parse error one
        // layer further from whatever actually went wrong.
        out = got > 0 ? (int)got : -1;
    }

    swHttpClose(h);

    // The one number that tells a "search found nothing" report apart from a
    // "search never reached the server" one. v1.0.4's log recorded that the
    // connection opened and then said nothing at all about what came back, so a
    // console reporting an empty result set could not be distinguished from one
    // whose body was truncated at `cap` or never arrived. Logged here rather
    // than in directory.c because directory.c is compiled on the host by the
    // `mirrors` suite, where swDiagf does not exist.
    swDiagf("GET %s -> %d bytes (cap=%u)", url, out, (unsigned)cap);

    return out;
}

// Shared body of swHttpGetText and swHttpGetTextBounded: mallocs the private
// handle those two promise, zeroes it (this is the one path here where `h` is
// fresh, possibly-garbage memory rather than a caller's zero-initialised
// struct - see get_text_into's comment), and frees it again on the way out.
static int get_text(const char *url, char *buf, size_t cap,
                    int head_total_ms, int body_idle_ms)
{
    SwHttp *h = (SwHttp *)malloc(sizeof(SwHttp));
    if (!h) return -1;
    memset(h, 0, sizeof(*h));

    int out = get_text_into(h, url, buf, cap, head_total_ms, body_idle_ms);

    free(h);
    return out;
}

int swHttpGetText(const char *url, char *buf, size_t cap)
{
    return get_text(url, buf, cap, HEAD_TOTAL_MS, BODY_IDLE_MS);
}

int swHttpGetTextBounded(const char *url, char *buf, size_t cap,
                         int head_total_ms, int body_idle_ms)
{
    return get_text(url, buf, cap, head_total_ms, body_idle_ms);
}

int swHttpGetTextBoundedH(SwHttp *h, const char *url, char *buf, size_t cap,
                          int head_total_ms, int body_idle_ms)
{
    return get_text_into(h, url, buf, cap, head_total_ms, body_idle_ms);
}
