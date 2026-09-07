#include "httpmsg.h"

#include "url.h"    // SW_URL_HOST_MAX and SwUrlParts::port - the hostline bound

#include <limits.h>   // LONG_MAX - the bound swHttpHeadInt accumulates against
#include <stdio.h>
#include <string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool eq_ci(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

// Finds the end of the head: CRLFCRLF, or LFLF from a server that never learned
// about CR. Returns the offset just past the blank line, or 0 if it is not
// there yet.
static size_t find_head_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n') {
            if (buf[i + 1] == '\n') return i + 2;
            // <= rather than <. i+3 is the offset one past the terminator, so
            // demanding it be a readable index demanded a body byte that need
            // not have arrived. That refused every CRLF head whose blank line
            // landed exactly on the end of a read - not a corner case - and
            // against a server that sends a head and then nothing, it waited
            // for a byte that was never coming. The LF branch above never had
            // the fault because i+2 really is the last byte it reads.
            if (i + 3 <= len && buf[i + 1] == '\r' && buf[i + 2] == '\n') return i + 3;
        }
    }
    return 0;
}

SwHeadState swHttpHeadParse(const char *buf, size_t len,
                            SwHttpHead *out, size_t *body_off)
{
    if (!buf || !out || !body_off) return SW_HEAD_BAD;

    size_t end = find_head_end(buf, len);
    if (end == 0) {
        // Not finished. Refuse before the caller's buffer grows without bound -
        // a server that never sends a blank line would otherwise hang us.
        return (len >= SW_HEAD_MAX) ? SW_HEAD_BAD : SW_HEAD_NEED_MORE;
    }
    if (end > SW_HEAD_MAX) return SW_HEAD_BAD;

    memset(out, 0, sizeof(*out));

    // Status line. Two shapes are accepted, and only two:
    //   HTTP/1.1 200 OK
    //   ICY 200 OK          (Shoutcast v1, still common in the directory)
    const char *p = buf;
    const char *stop = buf + end;

    if (eq_ci(p, "http/", 5) && (size_t)(stop - p) > 8) {
        p += 5;
        while (p < stop && *p != ' ') p++;      // skip the version
    } else if (eq_ci(p, "icy", 3) && (size_t)(stop - p) > 4) {
        out->icy = true;
        p += 3;
    } else {
        return SW_HEAD_BAD;
    }

    while (p < stop && *p == ' ') p++;
    if (p + 3 > stop || p[0] < '0' || p[0] > '9') return SW_HEAD_BAD;

    int status = 0;
    int digits = 0;
    while (p < stop && *p >= '0' && *p <= '9' && digits < 3) {
        status = status * 10 + (*p++ - '0');
        digits++;
    }
    // An HTTP status is three digits by definition. Anything else means this is
    // not a response head, whatever else it looked like.
    //
    // The fourth-digit check is the half that is easy to leave out: stopping at
    // three digits and asking no further questions read "HTTP/1.1 2000 OK" as a
    // perfectly good 200, which is a response head this app has no business
    // trusting - and for the updater, one it was about to install from.
    if (p < stop && *p >= '0' && *p <= '9') return SW_HEAD_BAD;
    if (digits != 3 || status < 100 || status > 599) return SW_HEAD_BAD;
    out->status = status;

    // Everything from the line after the status line up to the blank line is
    // kept verbatim for the header lookups.
    const char *nl = memchr(buf, '\n', end);
    if (!nl) return SW_HEAD_BAD;
    const char *hdr = nl + 1;

    size_t hdr_len = (size_t)(stop - hdr);
    if (hdr_len >= sizeof(out->raw)) return SW_HEAD_BAD;
    memcpy(out->raw, hdr, hdr_len);
    out->raw[hdr_len] = 0;
    out->raw_len = hdr_len;

    *body_off = end;
    return SW_HEAD_OK;
}

bool swHttpHeadValue(const SwHttpHead *h, const char *name,
                     char *out, size_t cap)
{
    if (!h || !name || !out || cap == 0) return false;

    size_t name_len = strlen(name);
    const char *p = h->raw;

    while (*p) {
        const char *line_end = strchr(p, '\n');
        size_t line_len = line_end ? (size_t)(line_end - p) : strlen(p);

        // Match "name:" at the very start of the line. Anchoring matters: a
        // header called "x-icy-metaint" must not answer a lookup for
        // "icy-metaint".
        if (line_len > name_len && p[name_len] == ':' && eq_ci(p, name, name_len)) {
            const char *v = p + name_len + 1;
            const char *v_end = p + line_len;
            while (v < v_end && (*v == ' ' || *v == '\t')) v++;
            while (v_end > v && (v_end[-1] == '\r' || v_end[-1] == ' ' ||
                                 v_end[-1] == '\t')) v_end--;

            size_t n = (size_t)(v_end - v);
            if (n + 1 > cap) return false;
            memcpy(out, v, n);
            out[n] = 0;
            return true;
        }

        if (!line_end) break;
        p = line_end + 1;
    }
    return false;
}

long swHttpHeadInt(const SwHttpHead *h, const char *name, long fallback)
{
    char v[32];
    if (!swHttpHeadValue(h, name, v, sizeof(v))) return fallback;

    const char *p = v;
    if (*p < '0' || *p > '9') return fallback;

    // Bounded rather than free-running. `out = out * 10 + digit` on a signed
    // long is undefined behaviour the moment it overflows, and UBSan measured
    // this loop doing exactly that on a header value a stream server can send
    // for free:
    //   source/net/httpmsg.c:153: runtime error: signed integer overflow:
    //   999999999999999999 * 10 cannot be represented in type 'long int'
    // A 21-digit value does it, and 31 digits fit in `v` above, so the buffer
    // bound was never the guard it looked like. On the console there is no
    // sanitizer to notice - the accumulation just wraps, and the caller is
    // handed a number with no relationship to what arrived.
    //
    // An overflowing value returns `fallback` rather than clamping to LONG_MAX.
    // Both live callers (source/net/http.c: icy-metaint and icy-br) cast the
    // result to `int`, and LONG_MAX does not survive that cast the same way on
    // both platforms - it truncates to -1 on the 64-bit host and stays
    // 2147483647 on the 3DS's 32-bit long - so a clamp would hand icyInit() the
    // negative metaint it was supposed to prevent, on the one platform where
    // nothing is watching. `fallback` is the single value every caller is
    // already written to handle, because it is what they get today for a header
    // that is not a number at all.
    long out = 0;
    while (*p >= '0' && *p <= '9') {
        int d = *p++ - '0';
        if (out > (LONG_MAX - d) / 10) return fallback;
        out = out * 10 + d;
    }
    return out;
}

bool swHttpHeadChunked(const SwHttpHead *h)
{
    char v[64];
    if (!swHttpHeadValue(h, "Transfer-Encoding", v, sizeof(v))) return false;

    // The value can be a list ("gzip, chunked"); chunked is required to be last
    // when present, and a substring match is enough for the only encoding this
    // app can decode.
    for (size_t i = 0; v[i]; i++) {
        if (lower(v[i]) == 'c' && strlen(v + i) >= 7 && eq_ci(v + i, "chunked", 7))
            return true;
    }
    return false;
}

bool swHttpIsHlsManifest(const SwHttpHead *h, const char *body, size_t body_len)
{
    if (h) {
        char ct[64];
        if (swHttpHeadValue(h, "Content-Type", ct, sizeof(ct))) {
            // Covers both spellings seen in the wild - RFC 8216's registered
            // "application/vnd.apple.mpegurl" and the widely-used
            // "application/x-mpegURL" - with one substring test rather than two
            // exact ones, since a server is free to add a charset parameter
            // after either.
            for (char *p = ct; *p; p++) *p = lower(*p);
            if (strstr(ct, "mpegurl")) return true;
        }
    }

    // RFC 8216 4.1: "Each Playlist file... MUST start with the tag #EXTM3U."
    // This is the one signal that cannot lie the way a header can, which is why
    // it is checked even when Content-Type already said yes, and is the only
    // thing checked when `h` is NULL.
    static const char magic[] = "#EXTM3U";
    if (body && body_len >= sizeof(magic) - 1 &&
        memcmp(body, magic, sizeof(magic) - 1) == 0)
        return true;

    return false;
}

// True if `s` carries a CR or an LF. A NULL argument has nothing to inject, so
// it is not an error here - the callers below already decide separately whether
// each optional field being absent is allowed.
static bool has_crlf(const char *s)
{
    return s && (strchr(s, '\r') || strchr(s, '\n'));
}

int swHttpBuildGet(char *out, size_t cap,
                   const char *host, const char *port, const char *path,
                   const char *user_agent, const char *authorization,
                   bool want_icy_meta, const char *extra)
{
    if (!out || cap == 0 || !host || !path) return -1;

    // Every string below is formatted into a header line or into the request
    // line itself, and a CR or LF in any of them closes that line early: what
    // follows it stops being a value and becomes a header of the sender's
    // choosing, carried on this app's own request. The station list comes from
    // radio-browser.info, which anyone can write to, so `host`, `port` and
    // `path` are attacker-controlled from end to end. The measured failure:
    //   swHttpBuildGet should refuse a host containing CR/LF; instead it built:
    //     GET /x HTTP/1.1
    //     Host: [evil.com
    //     X-Injected: pwned]
    //     User-Agent: UA
    // and the same again through `user_agent`.
    //
    // `user_agent` and `authorization` are the app's own constants today. They
    // are checked anyway for the reason the already-bracketed-host case further
    // down is written from: this function has no way to know who is calling it,
    // and a future caller folding a station name or a user-typed password into
    // either one must not be the thing that discovers this.
    //
    // Refused, never sanitised, which is this file's rule everywhere else - see
    // the oversized-host and truncated-hostline checks below. A request with
    // bytes quietly removed from its Host is not a safer request, it is a
    // correct-looking one sent to the wrong virtual host.
    //
    // `extra` is deliberately excluded: httpmsg.h's contract is that it already
    // *is* CRLF-terminated header lines, so CRLF is what it is for. It is the
    // caller's own literal and never carries directory data.
    if (has_crlf(host) || has_crlf(port) || has_crlf(path) ||
        has_crlf(user_agent) || has_crlf(authorization))
        return -1;

    // A non-default port belongs in the Host header; a default one must not be
    // there, because some CDNs vary their routing on the exact Host string.
    //
    // Sized from url.h, which is where both halves come from: a host is
    // SW_URL_HOST_MAX (256) bytes including its NUL, so at most 255 characters,
    // and SwUrlParts::port is 8 bytes, so at most 7. Worst case is therefore
    // 255 + '[' + ']' + ':' + 7 + NUL = 266. This used to be SW_HEAD_MAX (4096),
    // which is a quarter of the 16 KB network-thread stack (see
    // source/audio/player.c) for a buffer that can never hold more than a host
    // and a port.
    //
    // The two extra bytes over the plain host:port bound are for an IPv6
    // literal's brackets. swUrlSplit() hands this function the literal with its
    // brackets already stripped (SwUrlParts.host - see url.h: "no brackets,
    // even for an IPv6 literal"), so `host` here is "2001:db8::1", not
    // "[2001:db8::1]". RFC 7230 requires the brackets in the Host header - they
    // are what lets a server tell the address apart from the ":port" that may
    // follow it - so this function puts them back on rather than emitting the
    // unparseable "Host: 2001:db8::1:8000".
    char hostline[(SW_URL_HOST_MAX - 1)                          /* host chars */
                  + 2                                            /* '[' ']'    */
                  + 1                                            /* ':'        */
                  + (sizeof(((SwUrlParts *)0)->port) - 1)        /* port chars */
                  + 1];                                          /* NUL        */
    // The bound being enforced is url.h's, not this buffer's. A host longer
    // than swUrlSplit() can ever produce is refused outright rather than left
    // to land in the few spare bytes between 255 characters and the port's
    // share of the buffer, so the buffer below can only be handed something
    // that fits.
    if (strlen(host) >= SW_URL_HOST_MAX) return -1;

    // Detected by the ':' that cannot appear in a registered name or an IPv4
    // address - the same rule swUrlResolve() already uses to decide whether an
    // address it is rebuilding needs brackets (see url.c). A host that already
    // arrives bracketed (defensive - swUrlSplit() never hands one over that way,
    // but this function has no other way to know who called it) is left alone
    // rather than bracketed a second time.
    bool has_colon = strchr(host, ':') != NULL;
    bool needs_brackets = has_colon && host[0] != '[';

    bool default_port = !port || !*port ||
                        strcmp(port, "80") == 0 || strcmp(port, "443") == 0;
    int hn;
    if (needs_brackets) {
        hn = default_port
                 ? snprintf(hostline, sizeof(hostline), "[%s]", host)
                 : snprintf(hostline, sizeof(hostline), "[%s]:%s", host, port);
    } else {
        hn = default_port
                 ? snprintf(hostline, sizeof(hostline), "%s", host)
                 : snprintf(hostline, sizeof(hostline), "%s:%s", host, port);
    }
    // snprintf truncates silently, and a truncated Host is not a smaller
    // request - it is a correct-looking request sent to the wrong virtual host.
    // Refuse instead, the same rule swUrlSplit() applies to an oversized host.
    if (hn < 0 || (size_t)hn >= sizeof(hostline)) return -1;

    // Connection: close, deliberately. Keep-alive buys nothing here - the
    // directory fetch is one request and a stream never ends - and it removes a
    // whole class of "how many bytes are left" ambiguity from the reader.
    int n = snprintf(out, cap,
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: %s\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n",
                     path, hostline, user_agent ? user_agent : "Skywave");
    if (n < 0 || (size_t)n >= cap) return -1;

    if (authorization && *authorization) {
        int m = snprintf(out + n, cap - (size_t)n,
                         "Authorization: Basic %s\r\n", authorization);
        if (m < 0 || (size_t)(n + m) >= cap) return -1;
        n += m;
    }

    if (want_icy_meta) {
        int m = snprintf(out + n, cap - (size_t)n, "Icy-MetaData: 1\r\n");
        if (m < 0 || (size_t)(n + m) >= cap) return -1;
        n += m;
    }

    if (extra && *extra) {
        int m = snprintf(out + n, cap - (size_t)n, "%s", extra);
        if (m < 0 || (size_t)(n + m) >= cap) return -1;
        n += m;
    }

    int m = snprintf(out + n, cap - (size_t)n, "\r\n");
    if (m < 0 || (size_t)(n + m) >= cap) return -1;
    return n + m;
}

// ---- chunked transfer decoding -------------------------------------------

enum {
    CH_SIZE = 0,      // reading the hex length
    CH_SIZE_EOL,      // skipping to the end of the size line (extensions)
    CH_DATA,          // copying chunk bytes out
    CH_DATA_CR,       // the CR after a chunk's data
    CH_DATA_LF,       // the LF after a chunk's data
    CH_TRAILER,       // after the final chunk, discarding trailer lines
    CH_DEAD,          // malformed; every further call fails
};

void swChunkedInit(SwChunked *c)
{
    if (!c) return;
    c->state  = CH_SIZE;
    c->remain = 0;
    c->done   = false;
}

static int hexval(unsigned char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int swChunkedFeed(SwChunked *c, const unsigned char *in, size_t in_len,
                  size_t *consumed, unsigned char *out, size_t out_cap)
{
    if (!c || !in || !consumed || !out) return -1;

    size_t i = 0, o = 0;
    *consumed = 0;

    if (c->state == CH_DEAD) return -1;

    while (i < in_len) {
        switch (c->state) {
        case CH_SIZE: {
            int v = hexval(in[i]);
            if (v >= 0) {
                // A length field long enough to overflow is a malformed stream,
                // not a very large chunk.
                if (c->remain > (0xFFFFFFFFul >> 4)) { c->state = CH_DEAD; return -1; }
                c->remain = c->remain * 16 + (unsigned long)v;
                i++;
            } else if (in[i] == '\r' || in[i] == '\n' || in[i] == ';' || in[i] == ' ') {
                c->state = CH_SIZE_EOL;
            } else {
                c->state = CH_DEAD;
                return -1;
            }
            break;
        }

        case CH_SIZE_EOL:
            if (in[i] == '\n') {
                c->state = (c->remain == 0) ? CH_TRAILER : CH_DATA;
            }
            i++;
            break;

        case CH_DATA: {
            if (o == out_cap) goto out_full;
            size_t room  = out_cap - o;
            size_t avail = in_len - i;
            size_t n = c->remain < avail ? (size_t)c->remain : avail;
            if (n > room) n = room;

            memcpy(out + o, in + i, n);
            o += n;
            i += n;
            c->remain -= n;
            if (c->remain == 0) c->state = CH_DATA_CR;
            break;
        }

        case CH_DATA_CR:
            // Tolerate a missing CR: only the LF is load-bearing.
            if (in[i] == '\r') { i++; }
            c->state = CH_DATA_LF;
            break;

        case CH_DATA_LF:
            if (in[i] != '\n') { c->state = CH_DEAD; return -1; }
            i++;
            c->state = CH_SIZE;
            c->remain = 0;
            break;

        case CH_TRAILER:
            // The body is complete. Trailer headers and the final blank line are
            // consumed and discarded; nothing after this is body data.
            i++;
            c->done = true;
            break;

        default:
            c->state = CH_DEAD;
            return -1;
        }
    }

out_full:
    *consumed = i;
    return (int)o;
}
