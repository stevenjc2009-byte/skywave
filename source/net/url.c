#include "url.h"

#include <stdio.h>
#include <string.h>

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Case-insensitive prefix match that stops at `in`'s terminator on its own: NUL
// matches nothing in the pattern, so a string shorter than the prefix fails
// before reading past its end.
static bool starts_with_ci(const char *in, const char *lower_prefix)
{
    for (size_t i = 0; lower_prefix[i]; i++) {
        if (lower(in[i]) != lower_prefix[i]) return false;
    }
    return true;
}

// Copies `len` bytes into a fixed field, refusing rather than truncating.
static bool put(char *dst, size_t dst_cap, const char *src, size_t len)
{
    if (len + 1 > dst_cap) return false;
    memcpy(dst, src, len);
    dst[len] = 0;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// True for the bytes that may stand unescaped in a request target: RFC 3986's
// unreserved set and sub-delims, plus the three separators a path and query are
// built out of (':' '@' '/' '?') and the brackets that real query strings carry
// and every server tolerates.
//
// Everything else is escaped, which is what makes this worth having rather than
// listing only the obvious offenders. A space ends the request target early and
// the server reads the rest of the path as the HTTP version; a CR or LF ends the
// request *line* and turns the remainder of a directory-supplied address into
// headers of its own. Neither is a case to be enumerated - anything outside the
// allowed set is unsafe by construction.
static bool path_char_ok(unsigned char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;
    return strchr("-._~!$&'()*+,;=:@/?[]", (char)c) != NULL && c != 0;
}

// Copies a raw path into a fixed field, percent-encoding what may not appear
// literally and refusing rather than truncating. An existing valid `%XX` is
// copied through as-is, so running this over its own output changes nothing.
static bool put_path(char *dst, size_t cap, const char *src, size_t len,
                     bool lead_slash)
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t o = 0;

    if (lead_slash) {
        if (o + 1 >= cap) return false;
        dst[o++] = '/';
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];

        if (c == '%' && i + 2 < len &&
            hexval(src[i + 1]) >= 0 && hexval(src[i + 2]) >= 0) {
            if (o + 3 >= cap) return false;
            dst[o++] = '%';
            dst[o++] = src[i + 1];
            dst[o++] = src[i + 2];
            i += 2;
            continue;
        }

        if (path_char_ok(c)) {
            if (o + 1 >= cap) return false;
            dst[o++] = (char)c;
            continue;
        }

        if (o + 3 >= cap) return false;
        dst[o++] = '%';
        dst[o++] = kHex[c >> 4];
        dst[o++] = kHex[c & 0x0F];
    }

    dst[o] = 0;
    return true;
}

bool swUrlSplit(const char *in, SwUrlParts *out)
{
    if (!in || !out) return false;

    memset(out, 0, sizeof(*out));

    const char *p;
    if (starts_with_ci(in, "https://")) {
        out->tls = true;
        p = in + 8;
    } else if (starts_with_ci(in, "http://")) {
        out->tls = false;
        p = in + 7;
    } else {
        return false;
    }

    // The authority runs to the first '/', '?' or '#'. Everything after is the
    // path, and there may be none at all.
    const char *auth = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    const char *auth_end = p;

    // Credentials, if any. The '@' must be inside the authority: a path can
    // legitimately contain one and must not be mistaken for userinfo.
    const char *at = NULL;
    for (const char *q = auth; q < auth_end; q++) {
        if (*q == '@') at = q;          // last '@' wins, per RFC 3986
    }
    if (at) {
        if (!put(out->userinfo, sizeof(out->userinfo), auth, (size_t)(at - auth)))
            return false;
        auth = at + 1;
    }

    // Host, with an IPv6 literal's brackets stripped. The port is whatever
    // follows a ':' that is outside the brackets.
    const char *host = auth;
    const char *host_end;
    const char *colon = NULL;

    if (*host == '[') {
        const char *close = host;
        while (close < auth_end && *close != ']') close++;
        if (close >= auth_end) return false;       // unterminated literal
        host++;
        host_end = close;
        if (close + 1 < auth_end && close[1] == ':') colon = close + 1;
    } else {
        host_end = auth_end;
        for (const char *q = host; q < auth_end; q++) {
            if (*q == ':') { colon = q; host_end = q; break; }
        }
    }

    if (host_end == host) return false;            // empty host

    // A control byte in a hostname is never a real address, and one of them is
    // a request-forgery primitive: swHttpBuildGet writes this field straight
    // into "Host: %s\r\n", so a CR or LF here closes the Host line early and
    // the rest of a directory-supplied address becomes headers on the app's own
    // request. The measured failure was
    //   host came out as "evil.example\r\nX-Evil"
    // from "http://evil.example\r\nX-Evil:12345/path" - the digits after that
    // colon are what carried it past the port check below, so nothing else in
    // this function was ever going to catch it.
    //
    // Refused, not stripped, unlike the path a few lines down. A path's
    // offending bytes have a faithful escaped form, so encoding them keeps the
    // address the station meant; a hostname has no such form, and a host with
    // bytes quietly removed from it is a different host - connecting to it is
    // worse than not connecting. The refusal is visible: http.c turns a false
    // from here into SW_HTTP_ERR_OPEN, which the user reads as "Bad stream
    // address.", not a blank error.
    //
    // Space and DEL go with CR and LF because they are unusable in a Host
    // header for the same structural reason and cost nothing to exclude.
    for (const char *q = host; q < host_end; q++) {
        unsigned char c = (unsigned char)*q;
        if (c <= 0x20 || c == 0x7F) return false;
    }

    if (!put(out->host, sizeof(out->host), host, (size_t)(host_end - host)))
        return false;

    if (colon) {
        const char *ps = colon + 1;
        if (ps == auth_end) return false;          // "host:" with no number

        // Digits only, and inside the range a TCP port can actually hold. The
        // range check is not pedantry. For a literal-IP host the port string
        // never reaches getaddrinfo(), which would have rejected it; it goes
        // through htons((uint16_t)atoi(port)) in tcp.c's numeric_addrinfo
        // instead, and that cast silently wraps. A directory entry reading
        // "http://1.2.3.4:99999/stream" therefore used to connect to port
        // 34463 - a real connection to a destination the station never wrote.
        // Same rule as the host check above: inventing an address is worse
        // than refusing one, because a refusal is something the user can see.
        unsigned long value = 0;
        for (const char *q = ps; q < auth_end; q++) {
            if (*q < '0' || *q > '9') return false;
            value = value * 10 + (unsigned long)(*q - '0');
            if (value > 65535) return false;       // also caps the loop's growth
        }
        if (value == 0) return false;              // port 0 is not connectable

        if (!put(out->port, sizeof(out->port), ps, (size_t)(auth_end - ps)))
            return false;
    } else {
        snprintf(out->port, sizeof(out->port), "%s", out->tls ? "443" : "80");
    }

    // Path. A fragment is never sent to a server, so it is cut here rather than
    // being carried around and stripped later.
    const char *path = auth_end;
    size_t path_len = strlen(path);
    const char *hash = memchr(path, '#', path_len);
    if (hash) path_len = (size_t)(hash - path);

    if (path_len == 0) {
        // "http://host:8000" with nothing after it. The '/' is not cosmetic:
        // "GET  HTTP/1.1" with an empty target is a 400 from every server.
        snprintf(out->path, sizeof(out->path), "/");
        return true;
    }

    // "host?query" is legal and means the root with a query, so the '/' has to
    // be put back in front of it.
    return put_path(out->path, sizeof(out->path), path, path_len,
                    /*lead_slash*/ *path == '?');
}

bool swUrlBasicAuth(const char *in, char *out, size_t cap)
{
    if (!in || !out) return false;

    // Decoded first: '@', ':' and '/' can only appear in a URL's userinfo
    // escaped, and base64-ing "p%40ss" sends a password that is not the one the
    // station was given.
    char decoded[SW_URL_AUTH_MAX];
    size_t o = 0;

    for (size_t i = 0; in[i]; i++) {
        char c = in[i];

        // Reads in[i+1] before in[i+2], and both stop at the terminator: a
        // trailing bare '%' fails the first test and never looks past the end.
        if (c == '%' && hexval(in[i + 1]) >= 0 && hexval(in[i + 2]) >= 0) {
            c = (char)((hexval(in[i + 1]) << 4) | hexval(in[i + 2]));
            i += 2;
        }

        // A NUL in the middle of the credentials would silently truncate them
        // and send a shorter password than the one that was decoded.
        if (c == 0) return false;
        if (o + 1 >= sizeof(decoded)) return false;
        decoded[o++] = c;
    }
    decoded[o] = 0;

    return swUrlBase64(decoded, out, cap);
}

bool swUrlHostEq(const char *a, const char *b)
{
    if (!a || !b) return false;

    size_t i = 0;
    for (; a[i] && b[i]; i++) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return a[i] == 0 && b[i] == 0;
}

bool swUrlBase64(const char *in, char *out, size_t cap)
{
    static const char kAlpha[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!in || !out) return false;

    size_t n = strlen(in);
    size_t need = ((n + 2) / 3) * 4 + 1;
    if (need > cap) return false;

    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];

        out[o++] = kAlpha[(v >> 18) & 0x3F];
        out[o++] = kAlpha[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? kAlpha[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? kAlpha[v & 0x3F]        : '=';
    }
    out[o] = 0;
    return true;
}

bool swUrlResolve(const SwUrlParts *base, const char *loc, char *out, size_t cap)
{
    if (!base || !loc || !out || cap == 0 || !*loc) return false;

    // Already absolute - nothing to resolve against.
    if (starts_with_ci(loc, "http://") || starts_with_ci(loc, "https://")) {
        if (strlen(loc) + 1 > cap) return false;
        snprintf(out, cap, "%s", loc);
        return true;
    }

    const char *scheme = base->tls ? "https" : "http";

    // A default port is left out of the rebuilt URL so the result matches what
    // the address would have looked like written by hand.
    //
    // The brackets have to go back on an IPv6 literal. SwUrlParts.host stores it
    // stripped, which is what a socket call wants, but a URL is not a socket
    // call: "http://::1:8080/x" has no way to say where the address ends and the
    // port begins, so it is not a valid address at all. Detected by the ':' that
    // cannot appear in any other kind of host.
    char hostport[SW_URL_HOST_MAX + 12];
    const char *br_open = strchr(base->host, ':') ? "[" : "";
    const char *br_close = *br_open ? "]" : "";
    bool default_port = strcmp(base->port, base->tls ? "443" : "80") == 0;
    if (default_port)
        snprintf(hostport, sizeof(hostport), "%s%s%s",
                 br_open, base->host, br_close);
    else
        snprintf(hostport, sizeof(hostport), "%s%s%s:%s",
                 br_open, base->host, br_close, base->port);

    // "//host/path" - protocol-relative, keeps the current scheme.
    if (loc[0] == '/' && loc[1] == '/') {
        int n = snprintf(out, cap, "%s:%s", scheme, loc);
        return n > 0 && (size_t)n < cap;
    }

    if (loc[0] == '/') {
        int n = snprintf(out, cap, "%s://%s%s", scheme, hostport, loc);
        return n > 0 && (size_t)n < cap;
    }

    // Relative: replace the last path segment.
    const char *slash = strrchr(base->path, '/');
    size_t dir_len = slash ? (size_t)(slash - base->path) + 1 : 1;

    int n = snprintf(out, cap, "%s://%s%.*s%s",
                     scheme, hostport, (int)dir_len, base->path, loc);
    return n > 0 && (size_t)n < cap;
}

bool swUrlToPlainHttp(const char *in, char *out, size_t cap)
{
    static const char kHttps[] = "https://";
    static const char kHttp[]  = "http://";
    const size_t https_len = sizeof(kHttps) - 1;   // 8
    const size_t http_len  = sizeof(kHttp) - 1;    // 7

    if (!in || !out || cap == 0) return false;

    // Stops at the terminator on its own: NUL matches nothing in kHttps, so a
    // string shorter than the scheme fails before reading past its end.
    for (size_t i = 0; i < https_len; i++) {
        if (lower(in[i]) != kHttps[i]) return false;
    }

    const char *rest = in + https_len;

    // Checked before writing, so a URL that does not fit leaves `out` alone
    // rather than handing back a truncated address that would connect to the
    // wrong place - a silently wrong stream is worse than a refusal.
    if (strlen(rest) + http_len + 1 > cap) return false;

    snprintf(out, cap, "%s%s", kHttp, rest);
    return true;
}
