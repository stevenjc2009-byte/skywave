// Host tests for the https -> http rewrite.
//
// This runs on every stream the user plays that the console cannot reach over
// TLS, so the cases that matter are the ones that would produce a *plausible
// but wrong* address: a mangled scheme, a dropped port, a query string cut off
// at the buffer edge. Any of those would surface on the console as "could not
// connect", indistinguishable from the station simply being down.

#include "../source/net/url.h"

#include <stdio.h>
#include <string.h>

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

static void expect(const char *in, const char *want)
{
    char out[512];
    memset(out, '#', sizeof(out));
    bool ok = swUrlToPlainHttp(in, out, sizeof(out));

    if (!want) {
        CHECK(!ok, "%s should have been refused, got \"%s\"", in, ok ? out : "");
        return;
    }
    CHECK(ok, "%s should have been rewritten", in);
    if (ok) CHECK(strcmp(out, want) == 0, "%s -> \"%s\", wanted \"%s\"",
                  in, out, want);
}

static void test_real_station_urls(void)
{
    printf("the stations this exists for\n");

    // The one steve reported: blocked over TLS, serves 200 audio/mpeg on 80.
    expect("https://media-ssl.musicradio.com/Heart80sMP3",
           "http://media-ssl.musicradio.com/Heart80sMP3");

    expect("https://icecast.anon.fm/radio", "http://icecast.anon.fm/radio");
    expect("https://stream.zeno.fm/6n6ewddtad0uv",
           "http://stream.zeno.fm/6n6ewddtad0uv");
}

static void test_parts_are_preserved(void)
{
    printf("ports, queries and fragments survive intact\n");

    expect("https://host:8443/mount", "http://host:8443/mount");
    expect("https://h/m?a=1&b=2", "http://h/m?a=1&b=2");

    // A real zeno.fm address carries a signed token; losing any of it gives a
    // URL that resolves and then 403s.
    expect("https://stream-174.zeno.fm/q97eczydqrhvv?zt=eyJhbGciOiJIUzI1NiJ9.abc",
           "http://stream-174.zeno.fm/q97eczydqrhvv?zt=eyJhbGciOiJIUzI1NiJ9.abc");

    expect("https://host/", "http://host/");
    expect("https://", "http://");
}

static void test_scheme_matching(void)
{
    printf("only https is rewritten, in any case\n");

    expect("HTTPS://Host.COM/Path", "http://Host.COM/Path");
    expect("HtTpS://h/m", "http://h/m");

    // Already plain: nothing to do, and rewriting it would loop.
    expect("http://host/mount", NULL);

    expect("httpsx://host", NULL);
    expect("ftp://host/x", NULL);
    expect("https:/host", NULL);      // one slash - not the scheme
    expect("https", NULL);            // shorter than the scheme
    expect("", NULL);
    expect(NULL, NULL);
}

static void test_capacity(void)
{
    printf("a result that does not fit is refused, not truncated\n");

    const char *in = "https://host/mount";      // -> "http://host/mount", 17+1
    char out[18];

    memset(out, '#', sizeof(out));
    CHECK(swUrlToPlainHttp(in, out, sizeof(out)),
          "exactly enough room should succeed");
    CHECK(strcmp(out, "http://host/mount") == 0, "got \"%s\"", out);

    char tight[17];
    memset(tight, '#', sizeof(tight));
    CHECK(!swUrlToPlainHttp(in, tight, sizeof(tight)),
          "one byte short must be refused");
    CHECK(tight[0] == '#', "a refusal must not touch the buffer, got '%c'",
          tight[0]);

    char zero[4] = {'#', '#', '#', '#'};
    CHECK(!swUrlToPlainHttp(in, zero, 0), "cap 0 must be refused");
    CHECK(zero[0] == '#', "cap 0 must not write");
}

int main(void)
{
    printf("== url ==\n");
    test_real_station_urls();
    test_parts_are_preserved();
    test_scheme_matching();
    test_capacity();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
