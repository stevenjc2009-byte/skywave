// Which mirror does a query stop at, and why?
//
// `fetch()` in source/net/directory.c walks kHosts until one answers, and until
// now nothing had ever run it: directory.c includes http.h, which pulls in
// mbedtls, so no host suite could compile it. See tests/stub/http.h for how
// that is worked around - the network is stubbed with a counter, everything
// else is the shipping code.
//
// The bug this suite was written for: `fetch()` used to break only on
// SW_DIR_OK, so a body that parsed perfectly well but held no playable station
// fell through and gave the NEXT mirror a turn. Every host in kHosts mirrors
// the same database, so that retry could never return a different answer - it
// just spent another round trip and another 98,304-byte parse arriving at the
// same one. Harmless when the server guaranteed MP3-only rows, because a
// non-empty response always parsed to a non-empty list; ordinary the moment
// codec_playable() could empty a full response on its own.
//
// What must stay true, and is the more important half of this file: a
// TRUNCATED body must still try the next mirror. That retry is why the loop
// exists.
#include <stdio.h>
#include <string.h>

#include "../source/net/directory.h"

// ------------------------------------------------------------- the stub net

static int g_calls;
static const char *g_body;      // what mirror 1 answers
static const char *g_body2;     // what mirror 2 answers; NULL = same as mirror 1

int swHttpGetText(const char *url, char *buf, size_t cap)
{
    (void)url;
    const char *body = (g_calls == 0 || !g_body2) ? g_body : g_body2;
    g_calls++;
    size_t n = strlen(body);
    if (n >= cap) n = cap - 1;
    memcpy(buf, body, n);
    buf[n] = 0;
    return (int)n;
}

// The register-play ping. Never exercised here; present so directory.c links.
int swHttpGetTextBoundedH(SwHttp *h, const char *url, char *buf, size_t cap,
                          size_t head_cap, size_t line_cap)
{
    (void)h; (void)url; (void)buf; (void)cap; (void)head_cap; (void)line_cap;
    return 0;
}

// ---------------------------------------------------------------- harness

static int checks, failures;

static void check(int got, int want, const char *what)
{
    checks++;
    if (got != want) {
        failures++;
        printf("  FAIL %-52s got %d, want %d\n", what, got, want);
    }
}

// ---------------------------------------------------------------- fixtures

// Well-formed JSON, but not one station the app can decode. This is what a
// narrow filter over an OGG-heavy country really returns.
static const char kAllUnplayable[] =
    "[{\"name\":\"Ogg One\",\"url_resolved\":\"http://a/1\",\"codec\":\"OGG\",\"bitrate\":128},"
    "{\"name\":\"Ogg Two\",\"url_resolved\":\"http://a/2\",\"codec\":\"OGG\",\"bitrate\":96},"
    "{\"name\":\"Hls One\",\"url_resolved\":\"http://a/3\",\"codec\":\"UNKNOWN\",\"bitrate\":0}]";

// The server itself found nothing. Also empty, also unhelped by a retry.
static const char kServerEmpty[] = "[]";

// At least one playable station. The control: if this ever needs more than one
// mirror, the counter is measuring something other than what it claims to.
static const char kOnePlayable[] =
    "[{\"name\":\"Ogg One\",\"url_resolved\":\"http://a/1\",\"codec\":\"OGG\",\"bitrate\":128},"
    "{\"name\":\"Aac One\",\"url_resolved\":\"http://a/2\",\"codec\":\"AAC+\",\"bitrate\":64}]";

// Cut off mid-array - exactly what http.c hands back as a positive byte count
// when a mirror goes quiet part-way through the body.
static const char kTruncated[] =
    "[{\"name\":\"Half A Station\",\"url_resolved\":\"http://a/1\",\"codec\":\"MP3\"";

static int run(const char *body, const char *body2, SwDirResult *r_out, int *kept)
{
    static SwStationList list;
    swDirInit();
    g_calls = 0;
    g_body  = body;
    g_body2 = body2;
    SwDirResult r = swDirTopStations(&list);
    if (r_out) *r_out = r;
    if (kept)  *kept  = list.count;
    return g_calls;
}

int main(void)
{
    SwDirResult r;
    int kept, n;

    puts("mirrors: a response with a playable station stops at the first mirror");
    n = run(kOnePlayable, NULL, &r, &kept);
    check(n, 1, "mirrors tried when something is playable");
    check((int)r, (int)SW_DIR_OK, "result");
    check(kept, 1, "stations kept");

    puts("mirrors: a well-formed response with nothing playable does NOT retry");
    n = run(kAllUnplayable, NULL, &r, &kept);
    check(n, 1, "mirrors tried when nothing is playable");
    check((int)r, (int)SW_DIR_EMPTY, "result");
    check(kept, 0, "stations kept");

    puts("mirrors: a genuinely empty result set does NOT retry");
    n = run(kServerEmpty, NULL, &r, &kept);
    check(n, 1, "mirrors tried on an empty result set");
    check((int)r, (int)SW_DIR_EMPTY, "result");

    // The regression guard. If making SW_DIR_EMPTY break ever gets widened to
    // "break on anything", these two go red.
    puts("mirrors: a TRUNCATED body still falls through to the next mirror");
    n = run(kTruncated, kOnePlayable, &r, &kept);
    check(n, 2, "mirrors tried on a truncated body");
    check((int)r, (int)SW_DIR_OK, "the healthy mirror's answer is the result");
    check(kept, 1, "stations kept from the healthy mirror");

    puts("mirrors: both mirrors truncated reports a parse error, not emptiness");
    n = run(kTruncated, kTruncated, &r, &kept);
    check(n, 2, "mirrors tried when both are truncated");
    check((int)r, (int)SW_DIR_ERR_PARSE, "result");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
