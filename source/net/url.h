#pragma once

// URL surgery, kept free of any 3DS header so it can be proven on a PC.
//
// This is one function today, and it is here rather than inside http.c for one
// reason: it rewrites the address the app is about to connect to, and getting
// it subtly wrong - an off-by-one on the scheme, a truncated query string -
// would show up as "could not connect" on a console with no log, which is the
// hardest possible place to debug it.

#include <stdbool.h>
#include <stddef.h>

// Long enough for GitHub's signed release-asset redirects, which run well past
// a kilobyte. Everything that holds a URL uses this one number so a long
// redirect cannot be truncated by whichever buffer it happens to land in - a
// truncated URL does not fail, it connects somewhere else.
#define SW_URL_MAX 2048

#define SW_URL_HOST_MAX 256
#define SW_URL_AUTH_MAX 128

// A URL taken apart far enough to open a socket and write a request line.
typedef struct {
    bool tls;                          // scheme was https
    char host[SW_URL_HOST_MAX];        // no brackets, even for an IPv6 literal
    char port[8];                      // always filled: "80" or "443" by default
    char path[SW_URL_MAX];             // origin-form, '/'-led, request-target safe
    char userinfo[SW_URL_AUTH_MAX];    // "user:pass" if the URL carried one, else ""
} SwUrlParts;

// Splits an absolute http/https URL into the pieces a request needs.
//
// Returns false, leaving `out` undefined, if the scheme is missing or is
// anything but http/https, if the host is empty, or if any piece would not fit.
// Refusing is deliberate: a silently truncated host or path is a connection to
// the wrong place, which is far worse than a refusal a user can see.
//
// Understood, because real directory data contains all of them: mixed-case
// schemes, an explicit `:port`, `user:pass@` credentials, a bracketed IPv6
// literal, a missing path (which becomes "/"), and a query string or fragment
// hanging off the end. The fragment is dropped - it is never sent to a server.
//
// `out->path` comes back percent-encoded to whatever a request target is
// actually allowed to contain. Directory data is not a trusted source of
// well-formed URLs: station addresses really do arrive with a raw space, a
// non-ASCII byte or a `|` in them, and copying those onto the request line
// verbatim produced a request no server would accept. A byte that is already a
// valid `%XX` escape is passed through untouched, so the encoding is idempotent
// and a signed token that already contains escapes is not corrupted by being
// parsed twice - which is exactly what the redirect loop does to it.
bool swUrlSplit(const char *in, SwUrlParts *out);

// Base64 of `in`, for the Authorization header. Returns false if it would not
// fit in `cap` (including the terminator).
bool swUrlBase64(const char *in, char *out, size_t cap);

// Turns a URL's `userinfo` into the exact value swHttpBuildGet() wants for its
// `authorization` argument: the bare base64 of "user:pass", with NO "Basic "
// in front of it.
//
// The missing prefix is the entire point of this function existing. Builder and
// caller each used to supply the scheme token, and the header that went out
// read `Authorization: Basic Basic dXNlcjpwYXNz` - which no server can parse,
// so every station carrying credentials in its URL answered 401 (or 400, on the
// stricter ones). The rule is now stated in one place and pinned by a test.
//
// Percent-escapes in the userinfo are decoded first: a password containing '@',
// ':' or '/' can only appear in a URL escaped, and base64-ing the escaped form
// sends the wrong password. Returns false if `in` does not fit, if it decodes
// to contain a NUL, or if the result would not fit in `cap`.
bool swUrlBasicAuth(const char *in, char *out, size_t cap);

// Case-insensitive host equality, for deciding whether credentials may follow a
// redirect. Hostnames are case-insensitive, so a station that answers
// "Location: http://STREAM.example.com/mount" is still the same host and must
// keep its credentials - while a redirect to a CDN is a different host and must
// never be sent the origin's password.
bool swUrlHostEq(const char *a, const char *b);

// Turns whatever a Location header contained into an absolute URL in `out`.
//
// Servers send all three forms and a client has to cope with all three: a full
// URL, an absolute path ("/stream2"), or a relative one ("stream2"). Icecast in
// particular answers a mount redirect with a bare path. Returns false if the
// result would not fit, or if `loc` is empty.
bool swUrlResolve(const SwUrlParts *base, const char *loc, char *out, size_t cap);

// Rewrites `https://rest` into `http://rest` in `out`.
//
// Returns false, leaving `out` untouched, if `in` is not an https URL or if the
// result would not fit in `cap` (including the terminator). The scheme match is
// case-insensitive: RFC 3986 says schemes are, and directory data really does
// contain "HTTPS://".
//
// Why the app ever wants this: a station whose TLS cannot be completed at all
// often serves the identical mount over plain http, and dropping to it is the
// only thing that recovers those. http.c does the downgrade inline now, on the
// parsed parts rather than on the string, so this is the host-testable statement
// of the same rule - see the plain-http fallback in open_url.
bool swUrlToPlainHttp(const char *in, char *out, size_t cap);
