// Hostile-input robustness suite for every parser that touches bytes an
// attacker controls end to end: the radio-browser JSON directory response,
// station URLs, HTTP response framing (including chunked bodies), ICY
// in-band metadata, and the favourites on-disk format.
//
// The other host suites (test_url.c, test_httpmsg.c, test_icy.c,
// test_directory.c) prove these parsers do the right thing on well-formed and
// lightly-malformed input. This suite assumes nothing is well-formed: every
// fixture below is truncated at every byte offset, has its terminators
// removed, carries injection attempts, or is bit-flipped by a seeded fuzzer.
// A radio-browser mirror is a third party and a stream server is whatever the
// user typed a URL for - on a console with no memory protection worth the
// name, a parser bug in any of these five files is the app's entire attack
// surface.
//
// Style matches test_url.c / test_httpmsg.c: same CHECK macro, same
// "N checks, M failures" summary, same exit-code convention (0 iff zero
// failures). One thing is different: a lot of what follows is deliberately
// trying to crash the code under test, so every risky call runs inside a
// forked child (run_isolated() below). If the child is killed or exits
// non-zero, that is scored as a normal CHECK failure - with the exact input
// that did it dumped as hex, so it can be turned into a permanent regression
// fixture - instead of taking the rest of the suite down with it.
//
// Build with ASan+UBSan: see the standalone script this ships alongside
// (there is no source-level dependency on it; any compiler invocation that
// links these five .c files against this one works). tools/build_test_robust.sh
// in the delivery scratchpad is the one actually used to verify this file.

// strnlen() and mkdtemp() are POSIX/glibc extensions hidden by -std=c11's
// strict mode unless a feature-test macro asks for them explicitly.
#define _GNU_SOURCE

#include "../source/net/directory.h"
#include "../source/net/url.h"
#include "../source/net/httpmsg.h"
#include "../source/net/icy.h"
#include "../source/store/favourites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// --------------------------------------------------------- crash isolation
//
// A hostile-input suite that can be killed by the bug it is looking for is
// useless past the first bug. Every call below that might plausibly crash -
// deep recursion, a truncation landing on a token boundary, the fuzz loop -
// runs in a forked child. The parent scores a crash as a normal CHECK
// failure and prints the exact bytes that caused it, then keeps going.
//
// This also doubles as the mechanism for semantic (non-crash) safety
// checks inside the child: a child that notices an invariant is broken
// (e.g. a parsed field not NUL-terminated inside its bound) can print why
// and _exit(1) itself, and it is reported exactly like a crash would be.

static void print_hex(const char *label, const void *buf, size_t len)
{
    const unsigned char *b = (const unsigned char *)buf;
    size_t show = len < 4096 ? len : 4096;
    printf("    %s (%zu bytes):", label, len);
    for (size_t i = 0; i < show; i++) {
        if (i % 32 == 0) printf("\n      ");
        printf("%02x", b[i]);
    }
    if (show < len) printf(" ... (%zu more bytes)", len - show);
    printf("\n");
    fflush(stdout);
}

// Runs fn(ctx) in a forked child. Returns true if the child ran to
// completion and exited 0 - a parser returning failure counts as success
// here, since it is crashes and sanitizer traps this is guarding against,
// not the parser's own verdict on the input.
static bool run_isolated(void (*fn)(void *), void *ctx)
{
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return false; }
    if (pid == 0) {
        fn(ctx);
        _exit(0);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return false; }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

#define CHECK_NO_CRASH(fn, ctx, inputbuf, inputlen, ...)               \
    do {                                                               \
        checks++;                                                      \
        if (!run_isolated((fn), (ctx))) {                              \
            failures++;                                                \
            printf("  FAIL %s:%d  ", __FILE__, __LINE__);              \
            printf(__VA_ARGS__);                                       \
            printf("\n");                                              \
            print_hex("failing input", (inputbuf), (inputlen));        \
        }                                                              \
    } while (0)

// ------------------------------------------------------------- fixed-seed RNG
//
// A homegrown xorshift32 rather than libc rand(): the point of a "fixed
// seed" fuzz loop is that it reproduces byte-for-byte on any machine that
// builds this file, and libc's rand() sequence is not part of any contract.

#define FUZZ_SEED 0xC0FFEEu

static uint32_t g_rng;

static void rng_seed(uint32_t s) { g_rng = s ? s : 1; }

static uint32_t rng_next(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

static uint32_t rng_below(uint32_t n) { return n ? rng_next() % n : 0; }

// Copies `src` into `dst` (capped at dst_cap) and applies one random
// mutation: a bit flip, a truncation, or a one-byte insertion. Returns the
// resulting length.
static size_t mutate(const unsigned char *src, size_t src_len,
                      unsigned char *dst, size_t dst_cap)
{
    size_t len = src_len < dst_cap ? src_len : dst_cap;
    memcpy(dst, src, len);

    switch (rng_below(3)) {
        case 0:   // bit flip
            if (len > 0) dst[rng_below((uint32_t)len)] ^= (unsigned char)(1u << rng_below(8));
            break;
        case 1:   // truncate
            if (len > 0) len = rng_below((uint32_t)len + 1);
            break;
        case 2:   // insert a random byte at a random offset
            if (len + 1 <= dst_cap) {
                size_t at = rng_below((uint32_t)len + 1);
                memmove(dst + at + 1, dst + at, len - at);
                dst[at] = (unsigned char)rng_next();
                len++;
            }
            break;
    }
    return len;
}

// =============================================================================
// directory_parse.c - the JSON station-list parser, URL-encoder and query
// builder.
// =============================================================================

typedef struct { const char *json; size_t len; } JsonCtx;

static void do_parse_json(void *ctx)
{
    JsonCtx *c = (JsonCtx *)ctx;
    SwStationList out;
    SwDirResult r = swDirParseJson(c->json, c->len, &out);

    if (r == SW_DIR_OK || r == SW_DIR_EMPTY) {
        // The invariants the rest of the app relies on: the cap is never
        // exceeded, and every kept station's strings are terminated inside
        // their field, not merely inside the process's address space.
        if (out.count < 0 || out.count > SW_STATIONS_MAX) {
            printf("    child: station count %d outside 0..%d\n", out.count, SW_STATIONS_MAX);
            _exit(1);
        }
        for (int i = 0; i < out.count; i++) {
            if (strnlen(out.items[i].name, SW_STATION_NAME) >= SW_STATION_NAME ||
                strnlen(out.items[i].url,  SW_STATION_URL)  >= SW_STATION_URL  ||
                strnlen(out.items[i].uuid, SW_STATION_UUID) >= SW_STATION_UUID ||
                strnlen(out.items[i].country, SW_STATION_CC) >= SW_STATION_CC) {
                printf("    child: station %d has an unterminated field\n", i);
                _exit(1);
            }
            // The one thing swDirParseJson promises about what it keeps.
            if (!out.items[i].name[0] || !out.items[i].url[0]) {
                printf("    child: station %d was kept with no name or url\n", i);
                _exit(1);
            }
        }
    }
}

static void test_json_empty_and_tiny(void)
{
    printf("directory: empty input and one-byte inputs\n");

    JsonCtx c;
    c = (JsonCtx){ "", 0 };
    CHECK_NO_CRASH(do_parse_json, &c, "", 0, "empty input crashed the parser");

    static const char *tiny[] = { "[", "]", "{", "}", "\"", "0", "-", "\\", "\xFF", "\x00" };
    for (size_t i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
        c = (JsonCtx){ tiny[i], 1 };
        CHECK_NO_CRASH(do_parse_json, &c, tiny[i], 1,
                       "one-byte input 0x%02x crashed the parser", (unsigned char)tiny[i][0]);
    }
}

// A real-shaped multi-station fixture, deliberately smaller than the ones in
// test_directory.c so a full byte-by-byte truncation sweep stays cheap, but
// carrying the things that break a naive walker: nested arrays, escapes, a
// missing field, an out-of-order field, and a nested object.
static const char *kTruncFixture =
"["
 "{\"name\":\"Classic FM\",\"stationuuid\":\"uuid-1\",\"url_resolved\":"
  "\"http://media-ice.musicradio.com/ClassicFMMP3\",\"country\":\"UK\","
  "\"bitrate\":128,\"tags\":[\"classical\",\"news\"],\"geo\":{\"lat\":1,\"lon\":2}},"
 "{\"bitrate\":320,\"name\":\"Quo\\\"ted\\\\Name\",\"stationuuid\":\"uuid-2\","
  "\"url\":\"http://example.org/stream\"},"
 "{\"name\":\"Caf\\u00e9 Jazz\",\"url_resolved\":\"http://jazz.example/x\",\"country\":\"FR\"}"
"]";

static void test_json_truncation_sweep(void)
{
    size_t full_len = strlen(kTruncFixture);
    printf("directory: truncation of a %zu-byte fixture at every byte offset\n", full_len);

    for (size_t cut = 0; cut <= full_len; cut++) {
        JsonCtx c = { kTruncFixture, cut };
        CHECK_NO_CRASH(do_parse_json, &c, kTruncFixture, cut,
                       "fixture truncated at byte %zu crashed the parser", cut);
    }
}

// skip() (directory_parse.c) recurses once per level of JSON nesting with no
// depth limit of its own - only jsmn's TOKENS_MAX (6144) bounds how deep an
// attacker-controlled array can nest, because each level of "[" consumes one
// token. This is the input that actually reaches that depth.
static void test_json_deep_nesting(void)
{
    printf("directory: array nesting deep enough to stress skip()'s recursion\n");

    const int depth = 6000;   // just inside TOKENS_MAX, so jsmn itself still succeeds
    char *js = malloc((size_t)depth * 2 + 8);
    size_t o = 0;
    for (int i = 0; i < depth; i++) js[o++] = '[';
    for (int i = 0; i < depth; i++) js[o++] = ']';
    js[o] = 0;

    JsonCtx c = { js, o };
    CHECK_NO_CRASH(do_parse_json, &c, js, o,
                   "%d levels of array nesting crashed or overflowed the stack in skip()", depth);
    free(js);
}

static void test_json_many_stations(void)
{
    printf("directory: 10,000 stations in one array - must degrade gracefully, not crash\n");

    size_t cap = 2 * 1024 * 1024;
    char *js = malloc(cap);
    size_t o = 0;
    o += (size_t)snprintf(js + o, cap - o, "[");
    for (int i = 0; i < 10000 && o + 128 < cap; i++) {
        o += (size_t)snprintf(js + o, cap - o,
                              "%s{\"name\":\"S%d\",\"url_resolved\":\"http://s/%d\",\"bitrate\":128}",
                              i ? "," : "", i, i);
    }
    o += (size_t)snprintf(js + o, cap - o, "]");

    JsonCtx c = { js, o };
    CHECK_NO_CRASH(do_parse_json, &c, js, o < 256 ? o : 256,
                   "10,000-station array crashed the parser");

    // Not isolated - this one just checks the return value, which is only
    // interesting if the process is still alive to report it.
    SwStationList out;
    SwDirResult r = swDirParseJson(js, o, &out);
    printf("    10,000-station array (%zu bytes) -> result %d, %d stations kept\n", o, (int)r, out.count);
    CHECK(r == SW_DIR_OK || r == SW_DIR_ERR_PARSE,
          "unexpected result %d for an oversized-but-legal response", (int)r);
    CHECK(out.count <= SW_STATIONS_MAX, "count %d exceeds the cap", out.count);
    free(js);
}

static void test_json_huge_field(void)
{
    printf("directory: a 10,000-character station name is clipped, not overflowed\n");

    char *name = malloc(10001);
    memset(name, 'N', 10000);
    name[10000] = 0;

    char *js = malloc(10200);
    snprintf(js, 10200, "[{\"name\":\"%s\",\"url_resolved\":\"http://x/y\",\"bitrate\":1}]", name);

    SwStationList out;
    SwDirResult r = swDirParseJson(js, strlen(js), &out);
    CHECK(r == SW_DIR_OK, "should still parse, got %d", (int)r);
    if (r == SW_DIR_OK && out.count == 1) {
        CHECK(strlen(out.items[0].name) == SW_STATION_NAME - 1,
              "name should fill the field exactly, got %zu", strlen(out.items[0].name));
    }
    free(name);
    free(js);
}

// json_int() (directory_parse.c) has no bound on how many digits it will
// fold into `int v` via `v = v * 10 + digit` - an attacker-controlled
// "bitrate" field with enough digits drives a signed integer overflow, which
// is undefined behaviour in C and is exactly what UBSan is here to catch.
// See the report for file/line and the minimal patch.
static void do_parse_huge_bitrate(void *ctx)
{
    const char *js = (const char *)ctx;
    SwStationList out;
    swDirParseJson(js, strlen(js), &out);
}

static void test_json_bitrate_number_forms(void)
{
    printf("directory: bitrate field - overflow, sign, leading zeros\n");

    // 30 digits: guaranteed to overflow a 32-bit int well before the field
    // token ends, however many of those digits jsmn accepts as a PRIMITIVE.
    static char huge[256];
    snprintf(huge, sizeof(huge),
             "[{\"name\":\"x\",\"url_resolved\":\"http://x/y\",\"bitrate\":999999999999999999999999999999}]");
    CHECK_NO_CRASH(do_parse_huge_bitrate, huge, huge, strlen(huge),
                   "a 30-digit bitrate triggers undefined signed-integer overflow "
                   "in json_int() (source/net/directory_parse.c, ~line 381-390)");

    // Documented current behaviour, not a safety issue: a leading '-' or '+'
    // is not a digit, so json_int()'s loop exits immediately and the field is
    // silently read as 0 rather than as a negative/positive number or a
    // parse error.
    {
        SwStationList out;
        const char *js = "[{\"name\":\"x\",\"url_resolved\":\"http://x/y\",\"bitrate\":-5}]";
        CHECK(swDirParseJson(js, strlen(js), &out) == SW_DIR_OK, "should parse");
        CHECK(out.count == 1 && out.items[0].bitrate == 0,
              "a negative bitrate is silently read as 0 (documented, not asserted-safe): got %d",
              out.count == 1 ? out.items[0].bitrate : -999);
    }
    {
        SwStationList out;
        const char *js = "[{\"name\":\"x\",\"url_resolved\":\"http://x/y\",\"bitrate\":+5}]";
        // '+' is not valid JSON at all; jsmn's non-strict primitive scanner
        // still accepts it as a bare token, so this documents what actually
        // happens rather than what the JSON grammar says should.
        SwDirResult r = swDirParseJson(js, strlen(js), &out);
        CHECK(r == SW_DIR_OK || r == SW_DIR_ERR_PARSE, "unexpected result %d", (int)r);
    }
    {
        SwStationList out;
        const char *js = "[{\"name\":\"x\",\"url_resolved\":\"http://x/y\",\"bitrate\":007}]";
        CHECK(swDirParseJson(js, strlen(js), &out) == SW_DIR_OK, "should parse");
        CHECK(out.count == 1 && out.items[0].bitrate == 7,
              "leading zeros should still read as 7, got %d",
              out.count == 1 ? out.items[0].bitrate : -999);
    }
}

// CRLF and other control bytes inside a JSON string value are legal JSON (as
// long as they are not the two bytes JSON itself forbids raw in a string,
// 0x00-0x1F is technically illegal per strict JSON, but jsmn does not enforce
// that - see jsmn_parse_string, which only treats '"' and '\\' specially).
// The point of this check is not "should this be rejected" but "does it come
// out the other end intact and bounded, without corrupting whatever the app
// later does with the field" - swDirParseJson makes no promises about the
// content of `name`, only about it being NUL-terminated within its bound,
// which do_parse_json's child already checks for every case in this suite.
static void test_json_control_bytes_in_fields(void)
{
    printf("directory: CRLF and raw control bytes inside a string field\n");

    const char *js =
        "[{\"name\":\"evil\\r\\nX-Injected: 1\",\"url_resolved\":\"http://h/p\",\"bitrate\":1}]";
    SwStationList out;
    SwDirResult r = swDirParseJson(js, strlen(js), &out);
    CHECK(r == SW_DIR_OK, "should parse, got %d", (int)r);
    if (r == SW_DIR_OK && out.count == 1) {
        CHECK(strstr(out.items[0].name, "\r\n") != NULL,
              "the escaped CRLF should decode through to the field verbatim");
    }
}

// -------------------------------------------------------- swDirUrlEncode

static void do_encode_null(void *ctx)
{
    (void)ctx;
    char out[16];
    swDirUrlEncode(NULL, out, sizeof(out));   // no NULL check in the source
}

static void test_url_encode_hostile(void)
{
    printf("swDirUrlEncode: NULL input, zero/one-byte capacity, a 10,000-byte input\n");

    // swDirUrlEncode(), unlike its neighbours in this file, does not check
    // `in` for NULL before dereferencing it. Nothing in the current call
    // graph passes it a NULL - q_param() only calls it after checking
    // bounded_len() - but the function is exposed in directory.h "for
    // tests", i.e. as a public API, and it crashes if handed exactly what
    // its own header comment does not rule out.
    CHECK_NO_CRASH(do_encode_null, NULL, "(NULL)", 6,
                   "swDirUrlEncode(NULL, ...) crashes (no NULL check in source/net/directory_parse.c)");

    char out0[4] = { '#', '#', '#', '#' };
    swDirUrlEncode("abc", out0, 0);
    CHECK(out0[0] == '#', "cap 0 must not write anything, got '%c'", out0[0]);

    char out1[4] = { '#', '#', '#', '#' };
    swDirUrlEncode("abc", out1, 1);
    CHECK(out1[0] == 0, "cap 1 must produce an empty string, got '%c'", out1[0]);

    char *big = malloc(10001);
    memset(big, '&', 10000);   // worst case: every byte escapes to 3
    big[10000] = 0;
    char small[10];
    swDirUrlEncode(big, small, sizeof(small));
    CHECK(strlen(small) < sizeof(small), "truncation overran the buffer, len=%zu", strlen(small));
    CHECK(small[strlen(small) - 1] != '%' &&
          !(strlen(small) >= 2 && small[strlen(small) - 2] == '%'),
          "truncation must not leave a half-written %%XX escape: '%s'", small);
    free(big);
}

// -------------------------------------------------------- swDirBuildQuery

static void test_build_query_hostile(void)
{
    printf("swDirBuildQuery: injection attempts and boundary-length fields\n");

    SwDirFilter f;
    char q[SW_DIR_QUERY_MAX];

    // A field that is itself trying to smuggle a %00 or a raw '&'/'=' - both
    // must come out percent-encoded, never able to add or terminate a param.
    swDirFilterInit(&f);
    snprintf(f.name, sizeof(f.name), "%s", "x%00y&z=1");
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "must build");
    // '%' is itself outside the unreserved set, so a literal "%00" in the
    // field becomes "%2500" (the '%' escaped to %25, then the literal "00")
    // - not "%00", which would instead be misread by a server as an escaped
    // NUL. That distinction is the actual thing worth pinning here.
    CHECK(strstr(q, "&z=1") == NULL && strstr(q, "%2500y%26z%3D1") != NULL,
          "a literal %%00/&/= in a filter field leaked through unescaped: '%s'", q);

    // A field exactly SW_DIR_TEXT_MAX-1 bytes (the largest that still has a
    // terminator) must build; SW_DIR_TEXT_MAX bytes with none must be
    // refused. Both ends of the same off-by-one are covered here rather than
    // just the failing side, so a fix that makes both directions refuse
    // would still be caught.
    swDirFilterInit(&f);
    memset(f.name, 'A', SW_DIR_TEXT_MAX - 1);
    f.name[SW_DIR_TEXT_MAX - 1] = 0;
    CHECK(swDirBuildQuery(&f, q, sizeof(q)), "a name filling the field to its terminator must build");

    swDirFilterInit(&f);
    memset(f.name, 'A', SW_DIR_TEXT_MAX);   // no room for the terminator at all
    CHECK(!swDirBuildQuery(&f, q, sizeof(q)), "an unterminated max-length name must be refused");
}

// =============================================================================
// url.c - swUrlSplit, swUrlResolve, swUrlBasicAuth, swUrlHostEq
// =============================================================================

typedef struct { const char *s; } StrCtx;

static void do_url_split(void *ctx)
{
    StrCtx *c = (StrCtx *)ctx;
    SwUrlParts p;
    memset(&p, 0, sizeof(p));
    bool ok = swUrlSplit(c->s, &p);
    if (ok) {
        // The one invariant that matters for every downstream consumer:
        // every field is NUL-terminated inside its declared bound.
        if (strnlen(p.host, sizeof(p.host)) >= sizeof(p.host) ||
            strnlen(p.port, sizeof(p.port)) >= sizeof(p.port) ||
            strnlen(p.path, sizeof(p.path)) >= sizeof(p.path) ||
            strnlen(p.userinfo, sizeof(p.userinfo)) >= sizeof(p.userinfo)) {
            printf("    child: swUrlSplit produced an unterminated field\n");
            _exit(1);
        }
    }
}

static void test_url_split_empty_and_tiny(void)
{
    printf("url: empty input and one-byte inputs\n");

    StrCtx c = { "" };
    CHECK_NO_CRASH(do_url_split, &c, "", 0, "empty string crashed swUrlSplit");

    static const char *tiny[] = { "h", "/", ":", "@", "%", "[", "]", "\xFF" };
    for (size_t i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
        c = (StrCtx){ tiny[i] };
        CHECK_NO_CRASH(do_url_split, &c, tiny[i], 1, "one-byte input '%s' crashed swUrlSplit", tiny[i]);
    }

    CHECK(!swUrlSplit(NULL, &(SwUrlParts){0}), "NULL input must be refused, not crash");
}

static const char *kUrlTruncFixture =
    "https://user:p%40ss@[2001:db8::1]:8443/a/b%20c?x=1&y=2#frag";

static void test_url_split_truncation_sweep(void)
{
    size_t full_len = strlen(kUrlTruncFixture);
    printf("url: truncation of a %zu-byte URL at every byte offset\n", full_len);

    char buf[256];
    for (size_t cut = 0; cut <= full_len; cut++) {
        memcpy(buf, kUrlTruncFixture, cut);
        buf[cut] = 0;
        StrCtx c = { buf };
        CHECK_NO_CRASH(do_url_split, &c, buf, cut,
                       "URL truncated at byte %zu crashed swUrlSplit", cut);
    }
}

static void test_url_split_missing_pieces(void)
{
    printf("url: missing host, unterminated IPv6 literal, bare colon\n");

    static const char *bad[] = {
        "http://", "https://", "http:///path", "http://?x=1", "http://#f",
        "http://[", "http://[::1", "http://[::1/path", "http://[]/path",
        "http://:8000/path", "http://host:/path", "http://host:abc/path",
        "http://host:99999999999999999999/path",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        SwUrlParts p;
        CHECK(!swUrlSplit(bad[i], &p), "'%s' should have been refused", bad[i]);
    }
}

// swUrlSplit extracts the authority as everything up to the first '/', '?'
// or '#' - it never checks that substring for CR or LF. Downstream, that
// string is what swHttpBuildGet writes straight into a "Host: %s\r\n" line
// (see the swHttpBuildGet CRLF-injection check below, which is the sharper
// half of the same bug). This check demonstrates the first half: a directory
// entry with an embedded CRLF in the authority produces a `host` that still
// carries it, when the colon that follows happens to be followed only by
// digits (so the port-number check does not itself reject the input).
// Rewritten once the gap it described was closed. As first written this check
// asserted that the split SUCCEEDED - it pinned the bug in place as the
// precondition of its own diagnostic, so a fix that refused the URL outright
// made it fail. Refusal is the right answer: both stripping shapes invent an
// address the station never wrote. Cutting the authority at the CR connects to
// evil.example with the rest smuggled into the request target; deleting the
// bytes connects to "evil.exampleX-Evil" on port 12345. url.h's own rule is
// that a silently altered host is a connection to the wrong place, which is
// worse than a refusal the user can see - http.c renders this one as
// "Bad stream address."
static void test_url_split_host_crlf_refused(void)
{
    printf("url: swUrlSplit refuses CR/LF inside the host\n");

    SwUrlParts p;
    memset(&p, 0, sizeof(p));
    CHECK(!swUrlSplit("http://evil.example\r\nX-Evil:12345/path", &p),
          "a host carrying CR/LF must be refused outright "
          "(source/net/url.c, host extraction ~line 118-158)");

    // The control. Without it, a swUrlSplit that refused every URL would pass
    // the check above - which would make it a check that cannot go red.
    memset(&p, 0, sizeof(p));
    CHECK(swUrlSplit("http://good.example:12345/path", &p) &&
          strcmp(p.host, "good.example") == 0 &&
          strcmp(p.port, "12345") == 0,
          "an ordinary host of the same shape must still split");
}

static void test_url_split_userinfo_multiple_at(void)
{
    printf("url: userinfo with more than one '@' - last one wins, per RFC 3986\n");

    SwUrlParts p;
    CHECK(swUrlSplit("http://a@b@host/path", &p), "should split");
    CHECK(strcmp(p.userinfo, "a@b") == 0, "userinfo '%s', wanted 'a@b'", p.userinfo);
    CHECK(strcmp(p.host, "host") == 0, "host '%s', wanted 'host'", p.host);
}

// ------------------------------------------------------------ swUrlBasicAuth

static void do_basic_auth(void *ctx)
{
    StrCtx *c = (StrCtx *)ctx;
    char out[512];
    swUrlBasicAuth(c->s, out, sizeof(out));
}

static void test_basic_auth_hostile(void)
{
    printf("basicauth: dangling %% escapes, %%00, huge input\n");

    static const char *cases[] = {
        "", "%", "%0", "%00", "%zz", "user:%", "user:%0", "%40%40%40%40",
        "\xFF\xFE", "a:b:c:d@e@f",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        StrCtx c = { cases[i] };
        CHECK_NO_CRASH(do_basic_auth, &c, cases[i], strlen(cases[i]),
                       "swUrlBasicAuth('%s') crashed", cases[i]);
    }

    char out[8];
    CHECK(!swUrlBasicAuth(NULL, out, sizeof(out)), "NULL input must be refused");

    static char huge[4096];
    memset(huge, 'a', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    char out2[8192];
    CHECK(!swUrlBasicAuth(huge, out2, sizeof(out2)),
          "an input far longer than SW_URL_AUTH_MAX must be refused, not accepted");
}

// -------------------------------------------------------------- swUrlResolve

static void test_resolve_redirect_chain(void)
{
    printf("resolve: a 200-hop redirect chain never overruns a buffer or crashes\n");

    SwUrlParts cur = { .tls = false };
    snprintf(cur.host, sizeof(cur.host), "%s", "host");
    snprintf(cur.port, sizeof(cur.port), "%s", "80");
    snprintf(cur.path, sizeof(cur.path), "%s", "/start");

    for (int hop = 0; hop < 200; hop++) {
        char loc[64];
        // Alternates relative and absolute-path redirects, the two shapes an
        // Icecast mount-swap chain actually produces.
        snprintf(loc, sizeof(loc), (hop % 2) ? "/m%d" : "m%d", hop);

        char out[SW_URL_MAX];
        bool ok = swUrlResolve(&cur, loc, out, sizeof(out));
        CHECK(ok, "hop %d: resolve failed", hop);
        if (!ok) break;

        SwUrlParts next;
        CHECK(swUrlSplit(out, &next), "hop %d: resolved URL '%s' did not re-split", hop, out);
        cur = next;
    }
}

static void test_resolve_hostile(void)
{
    printf("resolve: NULL/empty locations and a location too long for the buffer\n");

    SwUrlParts base = {0};
    snprintf(base.host, sizeof(base.host), "host");
    snprintf(base.port, sizeof(base.port), "80");
    snprintf(base.path, sizeof(base.path), "/x");

    char out[16];
    CHECK(!swUrlResolve(&base, "", out, sizeof(out)), "empty location refused");
    CHECK(!swUrlResolve(&base, NULL, out, sizeof(out)), "NULL location refused");
    CHECK(!swUrlResolve(NULL, "y", out, sizeof(out)), "NULL base refused");
    CHECK(!swUrlResolve(&base, "y", NULL, sizeof(out)), "NULL out refused");
    CHECK(!swUrlResolve(&base, "y", out, 0), "zero cap refused");

    static char longloc[SW_URL_MAX * 2];
    memset(longloc, 'y', sizeof(longloc) - 1);
    longloc[sizeof(longloc) - 1] = 0;
    char out2[SW_URL_MAX];
    CHECK(!swUrlResolve(&base, longloc, out2, sizeof(out2)),
          "a location far longer than the buffer must be refused, not truncated");
}

// ------------------------------------------------------------- swUrlHostEq

static void test_host_eq_hostile(void)
{
    printf("hosteq: empty, NUL-adjacent, and wildly different lengths\n");

    CHECK(swUrlHostEq("", ""), "two empty hosts match");
    CHECK(!swUrlHostEq(NULL, NULL), "NULL,NULL must not crash or match true");

    static char a[300], b[300];
    memset(a, 'x', sizeof(a) - 1); a[sizeof(a) - 1] = 0;
    memset(b, 'x', sizeof(b) - 1); b[sizeof(b) - 1] = 0;
    CHECK(swUrlHostEq(a, b), "two identical 299-byte hosts must match");
    b[100] = 'Y';
    CHECK(!swUrlHostEq(a, b), "a single differing byte must break the match");
}

// -------------------------------------------------------------- url fuzzing

static void do_fuzz_url_split(void *ctx)
{
    unsigned char *buf = (unsigned char *)ctx;
    SwUrlParts p;
    swUrlSplit((const char *)buf, &p);
}

static void test_url_fuzz(void)
{
    printf("url: seeded fuzz loop over swUrlSplit (seed 0x%X)\n", FUZZ_SEED);
    rng_seed(FUZZ_SEED);

    unsigned char work[512];
    const int iters = 800;
    for (int i = 0; i < iters; i++) {
        size_t len = mutate((const unsigned char *)kUrlTruncFixture,
                             strlen(kUrlTruncFixture), work, sizeof(work) - 1);
        work[len] = 0;
        CHECK_NO_CRASH(do_fuzz_url_split, work, work, len,
                       "fuzz iteration %d crashed swUrlSplit", i);
    }
}

// =============================================================================
// httpmsg.c - swHttpHeadParse, header lookup, chunked decoding, swHttpBuildGet
// =============================================================================

typedef struct { const char *buf; size_t len; } BufCtx;

static void do_head_parse(void *ctx)
{
    BufCtx *c = (BufCtx *)ctx;
    SwHttpHead h;
    size_t body_off = (size_t)-1;
    SwHeadState st = swHttpHeadParse(c->buf, c->len, &h, &body_off);
    if (st == SW_HEAD_OK) {
        if (body_off > c->len) { printf("    child: body_off %zu > len %zu\n", body_off, c->len); _exit(1); }
        if (h.raw_len >= sizeof(h.raw)) { printf("    child: raw_len overruns raw[]\n"); _exit(1); }
        if (strnlen(h.raw, sizeof(h.raw)) != h.raw_len) {
            printf("    child: raw[] not terminated at raw_len\n");
            _exit(1);
        }
    }
}

static void test_head_empty_and_tiny(void)
{
    printf("httpmsg: empty input and one-byte inputs\n");

    BufCtx c = { "", 0 };
    CHECK_NO_CRASH(do_head_parse, &c, "", 0, "empty input crashed swHttpHeadParse");

    static const char *tiny[] = { "H", "\r", "\n", "\x00", "\xFF" };
    for (size_t i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
        c = (BufCtx){ tiny[i], 1 };
        CHECK_NO_CRASH(do_head_parse, &c, tiny[i], 1, "one-byte input crashed swHttpHeadParse");
    }

    SwHttpHead h; size_t off;
    CHECK(swHttpHeadParse(NULL, 0, &h, &off) == SW_HEAD_BAD, "NULL buf must be refused");
    CHECK(swHttpHeadParse("x", 1, NULL, &off) == SW_HEAD_BAD, "NULL out must be refused");
    CHECK(swHttpHeadParse("x", 1, &h, NULL) == SW_HEAD_BAD, "NULL body_off must be refused");
}

static const char *kHeadTruncFixture =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: audio/mpeg\r\n"
    "icy-metaint: 8192\r\n"
    "Transfer-Encoding: chunked\r\n"
    "X-Custom: a value with spaces\r\n"
    "\r\n"
    "5\r\nhello\r\n0\r\n\r\n";

static void test_head_truncation_sweep(void)
{
    size_t full_len = strlen(kHeadTruncFixture);
    printf("httpmsg: truncation of a %zu-byte head+body at every byte offset\n", full_len);

    for (size_t cut = 0; cut <= full_len; cut++) {
        BufCtx c = { kHeadTruncFixture, cut };
        CHECK_NO_CRASH(do_head_parse, &c, kHeadTruncFixture, cut,
                       "head+body truncated at byte %zu crashed swHttpHeadParse", cut);
    }
}

static void test_head_never_terminates(void)
{
    printf("httpmsg: a head that never finds its blank line, with embedded NULs\n");

    static char buf[SW_HEAD_MAX + 256];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "HTTP/1.1 200 OK\r\n");
    while (n < sizeof(buf) - 40) {
        buf[n++] = 'X'; buf[n++] = ':'; buf[n++] = ' ';
        buf[n++] = 0;              // an embedded NUL mid-header-line
        buf[n++] = 'a'; buf[n++] = '\r'; buf[n++] = '\n';
    }
    BufCtx c = { buf, n };
    CHECK_NO_CRASH(do_head_parse, &c, buf, n,
                   "an oversized head with embedded NULs and no terminator crashed the parser");

    SwHttpHead h; size_t off;
    CHECK(swHttpHeadParse(buf, n, &h, &off) == SW_HEAD_BAD,
          "an oversized unterminated head must be refused outright");
}

static void test_head_value_hostile(void)
{
    printf("header lookup: a value exactly at the caller's buffer bound, and one over\n");

    SwHttpHead h;
    memset(&h, 0, sizeof(h));
    snprintf(h.raw, sizeof(h.raw), "X-V: 0123456789\r\n");
    h.raw_len = strlen(h.raw);

    char exact[11];   // "0123456789" is 10 chars + NUL = 11
    CHECK(swHttpHeadValue(&h, "X-V", exact, sizeof(exact)), "exact-fit buffer should succeed");
    CHECK(strcmp(exact, "0123456789") == 0, "got '%s'", exact);

    char tight[10];
    tight[0] = '#';
    CHECK(!swHttpHeadValue(&h, "X-V", tight, sizeof(tight)), "one byte short must be refused");
    CHECK(tight[0] == '#', "a refusal must not touch the output buffer");

    CHECK(!swHttpHeadValue(&h, "X-V", exact, 0), "zero-capacity output must be refused");
    CHECK(!swHttpHeadValue(NULL, "X-V", exact, sizeof(exact)), "NULL head must be refused");
    CHECK(!swHttpHeadValue(&h, NULL, exact, sizeof(exact)), "NULL name must be refused");
}

static void do_head_int_huge(void *ctx)
{
    SwHttpHead *h = (SwHttpHead *)ctx;
    swHttpHeadInt(h, "X-Huge", -1);
}

static void test_head_int_number_forms(void)
{
    printf("swHttpHeadInt: sign, leading zeros, huge values, non-digits\n");

    SwHttpHead h;
    memset(&h, 0, sizeof(h));
    snprintf(h.raw, sizeof(h.raw),
             "X-Neg: -5\r\nX-Plus: +5\r\nX-Zeros: 007\r\nX-Huge: 999999999999999999999\r\n"
             "X-Mixed: 12a\r\n");
    h.raw_len = strlen(h.raw);

    CHECK(swHttpHeadInt(&h, "X-Neg", -1) == -1, "a leading '-' is not a digit: falls back, got %ld",
          swHttpHeadInt(&h, "X-Neg", -1));
    CHECK(swHttpHeadInt(&h, "X-Plus", -1) == -1, "a leading '+' is not a digit: falls back, got %ld",
          swHttpHeadInt(&h, "X-Plus", -1));
    CHECK(swHttpHeadInt(&h, "X-Zeros", -1) == 7, "leading zeros should still read as 7, got %ld",
          swHttpHeadInt(&h, "X-Zeros", -1));
    CHECK(swHttpHeadInt(&h, "X-Mixed", -1) == 12, "digits before the first non-digit are kept, got %ld",
          swHttpHeadInt(&h, "X-Mixed", -1));

    // swHttpHeadInt() (source/net/httpmsg.c, ~line 144-155) has exactly the
    // same defect as json_int() in directory_parse.c: it folds digits into a
    // signed `long` with `out = out*10 + digit` and no bound check. A header
    // value long enough overflows that accumulation, which is undefined
    // behaviour in C - so this runs through the same isolated-child harness
    // as everything else that can trip UBSan, rather than calling it
    // directly and risking the whole suite going down on one call.
    CHECK_NO_CRASH(do_head_int_huge, &h, h.raw, h.raw_len,
                   "a 21-digit header value triggers undefined signed-integer overflow "
                   "in swHttpHeadInt() (source/net/httpmsg.c, ~line 144-155)");
}

// ----------------------------------------------------------- chunked decoding

typedef struct { const unsigned char *in; size_t len; } ChunkCtx;

static void do_chunked_feed(void *ctx)
{
    ChunkCtx *c = (ChunkCtx *)ctx;
    SwChunked ch;
    swChunkedInit(&ch);
    unsigned char out[256];
    size_t consumed = 0;
    size_t total_in = 0;
    // Feed the whole thing byte-by-byte, the shape most likely to expose an
    // off-by-one at a state transition.
    while (total_in < c->len) {
        int n = swChunkedFeed(&ch, c->in + total_in, 1, &consumed, out, sizeof(out));
        if (n < 0) return;              // malformed: a clean refusal, not a bug
        total_in += consumed;
        if (consumed == 0 && n == 0) break;   // out full with a 1-byte input can't happen, but be safe
    }
}

static const unsigned char kChunkedFixture[] =
    "A\r\n0123456789\r\n"
    "5\r\nhello\r\n"
    "0\r\n\r\n";

static void test_chunked_truncation_sweep(void)
{
    size_t full_len = sizeof(kChunkedFixture) - 1;
    printf("httpmsg: truncation of a %zu-byte chunked body at every byte offset\n", full_len);

    for (size_t cut = 0; cut <= full_len; cut++) {
        ChunkCtx c = { kChunkedFixture, cut };
        CHECK_NO_CRASH(do_chunked_feed, &c, kChunkedFixture, cut,
                       "chunked body truncated at byte %zu crashed the decoder", cut);
    }
}

static void test_chunked_size_overflow(void)
{
    printf("httpmsg: a chunk-size field long enough to overflow must be refused, not wrapped\n");

    SwChunked c;
    swChunkedInit(&c);
    // 20 hex digits: on any platform this walks past the guard
    // (0xFFFFFFFFul >> 4) well before reaching a 64-bit wraparound, so a
    // correct implementation dies with -1 partway through, never having
    // treated the digit run as a small or negative chunk size.
    const unsigned char in[] = "FFFFFFFFFFFFFFFFFFFF\r\nAAAA";
    unsigned char out[64];
    size_t consumed = 0;
    int n = swChunkedFeed(&c, in, sizeof(in) - 1, &consumed, out, sizeof(out));
    CHECK(n == -1, "a 20-digit hex chunk size must be refused, got %d", n);

    // Missing CRLF terminators, one at a time.
    SwChunked c2; swChunkedInit(&c2);
    const unsigned char no_lf[] = "5\r\nhello\r";     // CR present, LF never arrives
    n = swChunkedFeed(&c2, no_lf, sizeof(no_lf) - 1, &consumed, out, sizeof(out));
    CHECK(n >= 0, "a chunk missing only its trailing LF should not itself be an error yet, got %d", n);

    SwChunked c3; swChunkedInit(&c3);
    const unsigned char bad_lf[] = "5\r\nhelloXX";     // no CR, no LF, garbage instead
    n = swChunkedFeed(&c3, bad_lf, sizeof(bad_lf) - 1, &consumed, out, sizeof(out));
    CHECK(n == -1, "garbage where the chunk terminator belongs must be refused, got %d", n);

    // A chunk-size line with a '+' - not a hex digit, not a delimiter.
    SwChunked c4; swChunkedInit(&c4);
    const unsigned char plus[] = "+5\r\nhello\r\n";
    n = swChunkedFeed(&c4, plus, sizeof(plus) - 1, &consumed, out, sizeof(out));
    CHECK(n == -1, "a '+' in the size field must be refused, got %d", n);
}

// ------------------------------------------------------------ swHttpBuildGet

// swHttpBuildGet formats `host`, `port` and `user_agent` straight into
// "Host: %s\r\n" / "User-Agent: %s\r\n" with no check for CR or LF in any of
// them. Nothing in this file's own call graph is exercised here - only the
// function's own contract, which is silent about what happens if any of
// those three ever originate from data an attacker had a hand in (a future
// caller passing a user-supplied station nickname as part of the User-Agent,
// say, or - see test_url_split_host_crlf_refused above - a host that
// reached here having already picked up a CRLF upstream). This is the
// sharper half of that same gap: it proves the injected line actually lands
// in the built request.
static void test_build_get_crlf_injection(void)
{
    printf("swHttpBuildGet: CR/LF in host or user-agent injects a header line into the request\n");

    char out[1024];
    int n = swHttpBuildGet(out, sizeof(out), "evil.com\r\nX-Injected: pwned", "80",
                           "/x", "UA", NULL, false, NULL);
    CHECK(n == -1,
          "swHttpBuildGet should refuse a host containing CR/LF; instead it built:\n%.*s"
          "\n    (source/net/httpmsg.c, swHttpBuildGet - host is never checked for '\\r'/'\\n')",
          n > 0 ? n : 0, n > 0 ? out : "");

    n = swHttpBuildGet(out, sizeof(out), "host", "80", "/x",
                       "UA\r\nX-Injected: pwned", NULL, false, NULL);
    CHECK(n == -1,
          "swHttpBuildGet should refuse a User-Agent containing CR/LF; instead it built:\n%.*s",
          n > 0 ? n : 0, n > 0 ? out : "");
}

static void test_build_get_hostile_lengths(void)
{
    printf("swHttpBuildGet: NULL required args, and a capacity of exactly zero\n");

    char out[256];
    CHECK(swHttpBuildGet(NULL, sizeof(out), "h", NULL, "/x", NULL, NULL, false, NULL) == -1,
          "NULL out must be refused");
    CHECK(swHttpBuildGet(out, 0, "h", NULL, "/x", NULL, NULL, false, NULL) == -1,
          "zero capacity must be refused");
    CHECK(swHttpBuildGet(out, sizeof(out), NULL, NULL, "/x", NULL, NULL, false, NULL) == -1,
          "NULL host must be refused");
    CHECK(swHttpBuildGet(out, sizeof(out), "h", NULL, NULL, NULL, NULL, false, NULL) == -1,
          "NULL path must be refused");

    static char huge_extra[8192];
    memset(huge_extra, 'a', sizeof(huge_extra) - 3);
    huge_extra[sizeof(huge_extra) - 3] = '\r';
    huge_extra[sizeof(huge_extra) - 2] = '\n';
    huge_extra[sizeof(huge_extra) - 1] = 0;
    CHECK(swHttpBuildGet(out, sizeof(out), "h", NULL, "/x", NULL, NULL, false, huge_extra) == -1,
          "an 'extra' block far longer than the buffer must be refused, not truncated");
}

// ------------------------------------------------------------- httpmsg fuzz

static void do_fuzz_head_parse(void *ctx)
{
    BufCtx *c = (BufCtx *)ctx;
    SwHttpHead h;
    size_t off;
    swHttpHeadParse(c->buf, c->len, &h, &off);
}

static void test_head_fuzz(void)
{
    printf("httpmsg: seeded fuzz loop over swHttpHeadParse (seed 0x%X)\n", FUZZ_SEED);
    rng_seed(FUZZ_SEED ^ 0x1111u);

    unsigned char work[1024];
    const int iters = 800;
    for (int i = 0; i < iters; i++) {
        size_t len = mutate((const unsigned char *)kHeadTruncFixture,
                             strlen(kHeadTruncFixture), work, sizeof(work));
        BufCtx c = { (const char *)work, len };
        CHECK_NO_CRASH(do_fuzz_head_parse, &c, work, len,
                       "fuzz iteration %d crashed swHttpHeadParse", i);
    }
}

// =============================================================================
// icy.c - icyParseTitle and the icyFeed demultiplexer
// =============================================================================

static void do_icy_parse_title(void *ctx)
{
    const char *meta = (const char *)ctx;
    char t[ICY_TITLE_MAX];
    icyParseTitle(meta, t, sizeof(t));
}

static void test_icy_title_empty_and_tiny(void)
{
    printf("icy: empty and one-byte metadata strings\n");

    CHECK_NO_CRASH(do_icy_parse_title, "", "", 0, "empty metadata crashed icyParseTitle");

    static const char *tiny[] = { "S", "'", "=", "\xFF" };
    for (size_t i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++)
        CHECK_NO_CRASH(do_icy_parse_title, (void *)tiny[i], tiny[i], 1,
                       "one-byte metadata crashed icyParseTitle");

    char t[8];
    CHECK(!icyParseTitle("StreamTitle='hi';", t, 0), "zero capacity must be refused");
}

static void test_icy_title_hostile(void)
{
    printf("icy: no closing quote, CRLF injection, a value exactly at the buffer bound\n");

    char t[ICY_TITLE_MAX];

    // No "';" anywhere - falls back to end of string, per the source comment.
    CHECK(icyParseTitle("StreamTitle='forever open", t, sizeof(t)), "should still parse");
    CHECK(strcmp(t, "forever open") == 0, "got '%s'", t);

    // A title that is itself trying to inject a CRLF. icyParseTitle has no
    // opinion on this - it copies bytes - so the check pins the current
    // (permissive) behaviour rather than asserting it is wrong: whatever
    // eventually puts a title on screen or in a log is where this would need
    // to be sanitised, not here.
    CHECK(icyParseTitle("StreamTitle='evil\r\nInjected';", t, sizeof(t)), "should parse");
    CHECK(strcmp(t, "evil\r\nInjected") == 0, "CRLF should pass through verbatim: '%s'", t);

    // A value exactly ICY_TITLE_MAX-1 bytes long: fills the buffer with no
    // truncation; one byte more must clip, not overflow.
    char *exact = malloc(ICY_TITLE_MAX);
    memset(exact, 'A', ICY_TITLE_MAX - 1);
    exact[ICY_TITLE_MAX - 1] = 0;
    char *meta1 = malloc(ICY_TITLE_MAX + 32);
    snprintf(meta1, ICY_TITLE_MAX + 32, "StreamTitle='%s';", exact);
    CHECK(icyParseTitle(meta1, t, sizeof(t)), "should parse");
    CHECK(strlen(t) == ICY_TITLE_MAX - 1, "expected an exact fill, got len %zu", strlen(t));

    char *over = malloc(ICY_TITLE_MAX + 1);
    memset(over, 'B', ICY_TITLE_MAX);
    over[ICY_TITLE_MAX] = 0;
    char *meta2 = malloc(ICY_TITLE_MAX + 32);
    snprintf(meta2, ICY_TITLE_MAX + 32, "StreamTitle='%s';", over);
    CHECK(icyParseTitle(meta2, t, sizeof(t)), "should parse");
    CHECK(strlen(t) == sizeof(t) - 1, "one byte over must clip to cap-1, got len %zu", strlen(t));

    free(exact); free(meta1); free(over); free(meta2);
}

typedef struct { const uint8_t *in; size_t len; int metaint; } IcyCtx;

static void do_icy_feed(void *ctx)
{
    IcyCtx *c = (IcyCtx *)ctx;
    IcyDemux d;
    icyInit(&d, c->metaint);
    uint8_t *out = malloc(c->len ? c->len : 1);
    size_t total_produced = 0;

    // Feed one byte at a time, the shape most likely to expose an off-by-one
    // at a state transition (audio/len/meta boundaries).
    for (size_t i = 0; i < c->len; i++) {
        size_t produced = icyFeed(&d, c->in + i, 1, out + total_produced);
        // icyFeed's own contract: never more output bytes than input bytes.
        if (produced > 1) { printf("    child: icyFeed produced %zu bytes from 1\n", produced); free(out); _exit(1); }
        total_produced += produced;
        if (total_produced > c->len) {
            printf("    child: icyFeed produced more total bytes (%zu) than fed (%zu)\n",
                   total_produced, c->len);
            free(out);
            _exit(1);
        }
    }
    free(out);
}

static void test_icy_feed_empty_and_tiny(void)
{
    printf("icy: icyFeed with empty input and various metaint values\n");

    static const int metaints[] = { -1, 0, 1, 255, 256, 4080, 4081, 65535 };
    for (size_t i = 0; i < sizeof(metaints) / sizeof(metaints[0]); i++) {
        IcyCtx c = { (const uint8_t *)"", 0, metaints[i] };
        CHECK_NO_CRASH(do_icy_feed, &c, "", 0,
                       "empty input with metaint=%d crashed icyFeed", metaints[i]);
    }
}

// Builds one audio-run + metadata-block stream, byte-exact per the format
// documented in icy.h.
static size_t build_icy_stream(unsigned char *out, int metaint, int len_byte, const char *title)
{
    size_t o = 0;
    for (int i = 0; i < metaint; i++) out[o++] = (unsigned char)(i & 0xFF);
    out[o++] = (unsigned char)len_byte;
    if (len_byte > 0) {
        char text[4096];
        size_t tlen = 0;
        if (title) tlen = (size_t)snprintf(text, sizeof(text), "StreamTitle='%s';StreamUrl='';", title);
        size_t pad = (size_t)len_byte * 16;
        size_t copy = tlen < pad ? tlen : pad;
        memcpy(out + o, text, copy);
        memset(out + o + copy, 0, pad - copy);
        o += pad;
    }
    for (int i = 0; i < 20; i++) out[o++] = (unsigned char)((metaint + i) & 0xFF);
    return o;
}

static void test_icy_feed_truncation_sweep(void)
{
    printf("icy: truncation of a full audio+metadata block at every byte offset\n");

    unsigned char stream[8192];
    size_t full_len = build_icy_stream(stream, 100, 255, "Truncation Target");
    printf("    fixture is %zu bytes (metaint=100, len_byte=255 -> %d-byte block)\n", full_len, 255 * 16);

    for (size_t cut = 0; cut <= full_len; cut++) {
        IcyCtx c = { stream, cut, 100 };
        CHECK_NO_CRASH(do_icy_feed, &c, stream, cut,
                       "icy stream truncated at byte %zu crashed icyFeed", cut);
    }
}

static void test_icy_feed_length_byte_boundaries(void)
{
    printf("icy: every length-byte value from 0 to 255, at each end of the range\n");

    static const int lens[] = { 0, 1, 2, 127, 128, 254, 255 };
    for (size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        unsigned char stream[8192];
        size_t n = build_icy_stream(stream, 50, lens[i], "x");
        IcyCtx c = { stream, n, 50 };
        CHECK_NO_CRASH(do_icy_feed, &c, stream, n,
                       "length byte %d crashed icyFeed", lens[i]);
    }
}

static void do_fuzz_icy_feed(void *ctx)
{
    IcyCtx *c = (IcyCtx *)ctx;
    IcyDemux d;
    icyInit(&d, c->metaint);
    uint8_t *out = malloc(c->len ? c->len : 1);
    icyFeed(&d, c->in, c->len, out);
    free(out);
}

static void test_icy_fuzz(void)
{
    printf("icy: seeded fuzz loop over icyFeed (seed 0x%X)\n", FUZZ_SEED);
    rng_seed(FUZZ_SEED ^ 0x2222u);

    unsigned char base[8192];
    size_t base_len = build_icy_stream(base, 100, 255, "Fuzz Base Title \xC3\xA9");

    unsigned char work[8192];
    const int iters = 600;
    for (int i = 0; i < iters; i++) {
        size_t len = mutate(base, base_len, work, sizeof(work));
        int metaint = (int)(rng_next() % 500) - 1;   // includes -1..0 and small positives
        IcyCtx c = { work, len, metaint };
        CHECK_NO_CRASH(do_fuzz_icy_feed, &c, work, len,
                       "fuzz iteration %d (metaint=%d) crashed icyFeed", i, metaint);
    }
}

// =============================================================================
// favourites.c - the on-disk TSV parser
//
// FAV_PATH is the literal string "sdmc:/3ds/skywave/favourites.tsv". On the
// 3DS that is an absolute libctru sdmc: path; on a POSIX host, ':' is just an
// ordinary filename character, so fopen() treats the whole thing as a path
// *relative to the process's current directory*: "./sdmc:/3ds/skywave/
// favourites.tsv". That accident is what makes it possible to exercise the
// real, unmodified swFavLoad()/swFavAdd()/swFavList() here at all: chdir into
// a scratch directory, write hostile bytes to that relative path ourselves,
// and call the real loader.
// =============================================================================

static char g_fav_dir[512];

static void fav_write(const char *bytes, size_t len)
{
    chdir(g_fav_dir);
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/skywave", 0777);
    FILE *f = fopen("sdmc:/3ds/skywave/favourites.tsv", "wb");
    if (f) { fwrite(bytes, 1, len, f); fclose(f); }
}

typedef struct { const char *bytes; size_t len; } FavCtx;

static void do_fav_load(void *ctx)
{
    FavCtx *c = (FavCtx *)ctx;
    fav_write(c->bytes, c->len);
    swFavLoad();
    const SwStationList *l = swFavList();

    if (l->count < 0 || l->count > SW_STATIONS_MAX) {
        printf("    child: favourites count %d outside 0..%d\n", l->count, SW_STATIONS_MAX);
        _exit(1);
    }
    for (int i = 0; i < l->count; i++) {
        if (strnlen(l->items[i].name, SW_STATION_NAME) >= SW_STATION_NAME ||
            strnlen(l->items[i].url, SW_STATION_URL) >= SW_STATION_URL ||
            strnlen(l->items[i].uuid, SW_STATION_UUID) >= SW_STATION_UUID ||
            strnlen(l->items[i].country, SW_STATION_CC) >= SW_STATION_CC) {
            printf("    child: favourite %d has an unterminated field\n", i);
            _exit(1);
        }
        if (!l->items[i].name[0] || !l->items[i].url[0]) {
            printf("    child: favourite %d was kept with no name or url\n", i);
            _exit(1);
        }
    }
}

static void setup_fav_scratch_dir(void)
{
    snprintf(g_fav_dir, sizeof(g_fav_dir), "/tmp/skywave_test_robust_fav_XXXXXX");
    if (!mkdtemp(g_fav_dir)) { perror("mkdtemp"); exit(1); }
}

static void test_fav_empty_and_tiny(void)
{
    printf("favourites: an empty file, and one-byte files\n");

    FavCtx c = { "", 0 };
    CHECK_NO_CRASH(do_fav_load, &c, "", 0, "an empty favourites.tsv crashed swFavLoad");

    static const char *tiny[] = { "\t", "\n", "x", "\xFF" };
    for (size_t i = 0; i < sizeof(tiny) / sizeof(tiny[0]); i++) {
        c = (FavCtx){ tiny[i], 1 };
        CHECK_NO_CRASH(do_fav_load, &c, tiny[i], 1, "a one-byte favourites.tsv crashed swFavLoad");
    }
}

static const char *kFavTruncFixture =
    "Classic FM\thttp://media-ice.musicradio.com/ClassicFMMP3\tuuid-1\tGB\t128\n"
    "Weird\tName\twith\ttabs\thttp://example.org/s\tuuid-2\tDE\t320\n"
    "Caf\xC3\xA9 Jazz\thttp://jazz.example/x\tuuid-3\tFR\t96\n";

static void test_fav_truncation_sweep(void)
{
    size_t full_len = strlen(kFavTruncFixture);
    printf("favourites: truncation of a %zu-byte file at every byte offset\n", full_len);

    for (size_t cut = 0; cut <= full_len; cut++) {
        FavCtx c = { kFavTruncFixture, cut };
        CHECK_NO_CRASH(do_fav_load, &c, kFavTruncFixture, cut,
                       "favourites.tsv truncated at byte %zu crashed swFavLoad", cut);
    }
}

// swFavLoad() reads each line with fgets() into a fixed stack buffer sized
// from the four field widths. A single field longer than that whole buffer
// has no newline anywhere inside the first read, so fgets() hands back a
// buffer-sized *fragment* of the oversized field and swFavLoad() parses it
// as if it were a complete record; the next fgets() call then resumes in the
// middle of the same real line and parses whatever tab-separated garbage
// happens to fall out of it as a second "record". Nothing overflows - this
// is a data-integrity gap (a corrupted or hand-edited file desyncs itself
// for the rest of that line), not a memory-safety one, so it is checked for
// crash-freedom and the resulting record count is reported rather than
// asserted, since "how many bogus phantom rows a mis-split line produces" is
// not something the source promises a specific answer to.
static void test_fav_overlong_field(void)
{
    printf("favourites: a single field far longer than the fgets() line buffer\n");

    size_t hugelen = 5000;
    char *huge = malloc(hugelen + 1);
    memset(huge, 'N', hugelen);
    huge[hugelen] = 0;

    char *file = malloc(hugelen + 256);
    size_t n = (size_t)snprintf(file, hugelen + 256,
                                "%s\thttp://example.org/x\tuuid-a\tGB\t128\n"
                                "Second Station\thttp://example.org/y\tuuid-b\tDE\t64\n",
                                huge);

    FavCtx c = { file, n };
    CHECK_NO_CRASH(do_fav_load, &c, file, n,
                   "a 5000-byte field in favourites.tsv crashed swFavLoad");

    fav_write(file, n);
    swFavLoad();
    const SwStationList *l = swFavList();
    printf("    a %zu-byte name produced %d parsed record(s) from what should be 2 real lines\n",
           hugelen, l->count);

    free(huge);
    free(file);
}

static void test_fav_bitrate_number_forms(void)
{
    printf("favourites: a bitrate field with 20 digits (atoi overflow) must not crash\n");

    const char *file =
        "Overflow Station\thttp://x/y\tuuid-1\tGB\t99999999999999999999\n";
    FavCtx c = { file, strlen(file) };
    CHECK_NO_CRASH(do_fav_load, &c, file, strlen(file),
                   "a 20-digit bitrate field crashed swFavLoad (atoi overflow)");
}

static void do_fuzz_fav_load(void *ctx)
{
    FavCtx *c = (FavCtx *)ctx;
    do_fav_load(c);
}

static void test_fav_fuzz(void)
{
    printf("favourites: seeded fuzz loop over swFavLoad (seed 0x%X)\n", FUZZ_SEED);
    rng_seed(FUZZ_SEED ^ 0x3333u);

    unsigned char work[2048];
    const int iters = 500;
    for (int i = 0; i < iters; i++) {
        size_t len = mutate((const unsigned char *)kFavTruncFixture,
                             strlen(kFavTruncFixture), work, sizeof(work));
        FavCtx c = { (const char *)work, len };
        CHECK_NO_CRASH(do_fuzz_fav_load, &c, work, len,
                       "fuzz iteration %d crashed swFavLoad", i);
    }
}

// =============================================================================

int main(void)
{
    printf("== robust ==\n");
    printf("fixed fuzz seed: 0x%X (deterministic - rerun to reproduce any failure)\n\n", FUZZ_SEED);

    printf("-- directory_parse.c --\n");
    test_json_empty_and_tiny();
    test_json_truncation_sweep();
    test_json_deep_nesting();
    test_json_many_stations();
    test_json_huge_field();
    test_json_bitrate_number_forms();
    test_json_control_bytes_in_fields();
    test_url_encode_hostile();
    test_build_query_hostile();

    printf("\n-- url.c --\n");
    test_url_split_empty_and_tiny();
    test_url_split_truncation_sweep();
    test_url_split_missing_pieces();
    test_url_split_host_crlf_refused();
    test_url_split_userinfo_multiple_at();
    test_basic_auth_hostile();
    test_resolve_redirect_chain();
    test_resolve_hostile();
    test_host_eq_hostile();
    test_url_fuzz();

    printf("\n-- httpmsg.c --\n");
    test_head_empty_and_tiny();
    test_head_truncation_sweep();
    test_head_never_terminates();
    test_head_value_hostile();
    test_head_int_number_forms();
    test_chunked_truncation_sweep();
    test_chunked_size_overflow();
    test_build_get_crlf_injection();
    test_build_get_hostile_lengths();
    test_head_fuzz();

    printf("\n-- icy.c --\n");
    test_icy_title_empty_and_tiny();
    test_icy_title_hostile();
    test_icy_feed_empty_and_tiny();
    test_icy_feed_truncation_sweep();
    test_icy_feed_length_byte_boundaries();
    test_icy_fuzz();

    printf("\n-- favourites.c --\n");
    setup_fav_scratch_dir();
    test_fav_empty_and_tiny();
    test_fav_truncation_sweep();
    test_fav_overlong_field();
    test_fav_bitrate_number_forms();
    test_fav_fuzz();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
