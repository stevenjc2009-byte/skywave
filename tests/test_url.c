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

// ---------------------------------------------------------------------------
// swUrlSplit
//
// This is the parser that decides which host the console actually opens a
// socket to. A parser bug here is not a cosmetic wrong string - it is a
// connection to somewhere the user did not ask for, so the interesting cases
// are the ones a lazy implementation gets subtly wrong: an '@' inside a path
// mistaken for credentials, a truncated host that still "succeeds", a query
// string with no path in front of it.

static void expect_split_ok(const char *in, bool want_tls, const char *want_host,
                             const char *want_port, const char *want_path,
                             const char *want_userinfo)
{
    SwUrlParts p;
    memset(&p, '#', sizeof(p));
    bool ok = swUrlSplit(in, &p);

    CHECK(ok, "%s should have split", in);
    if (!ok) return;

    CHECK(p.tls == want_tls, "%s: tls %d, wanted %d", in, p.tls, want_tls);
    CHECK(strcmp(p.host, want_host) == 0, "%s: host \"%s\", wanted \"%s\"",
          in, p.host, want_host);
    CHECK(strcmp(p.port, want_port) == 0, "%s: port \"%s\", wanted \"%s\"",
          in, p.port, want_port);
    CHECK(strcmp(p.path, want_path) == 0, "%s: path \"%s\", wanted \"%s\"",
          in, p.path, want_path);
    CHECK(strcmp(p.userinfo, want_userinfo) == 0,
          "%s: userinfo \"%s\", wanted \"%s\"", in, p.userinfo, want_userinfo);
}

static void expect_split_fail(const char *in)
{
    SwUrlParts p;
    memset(&p, '#', sizeof(p));
    CHECK(!swUrlSplit(in, &p), "%s should have been refused", in);
}

static void test_split_schemes_and_ports(void)
{
    printf("split: scheme, defaults and explicit ports\n");

    expect_split_ok("http://host/path", false, "host", "80", "/path", "");
    expect_split_ok("https://host/path", true, "host", "443", "/path", "");
    expect_split_ok("HTTPS://host/path", true, "host", "443", "/path", "");
    expect_split_ok("http://host:8000/path", false, "host", "8000", "/path", "");

    expect_split_fail("ftp://host/path");
    expect_split_fail("gopher://host");
    expect_split_fail("httpsx://host/path");
}

static void test_split_userinfo(void)
{
    printf("split: userinfo, and an '@' in the path that must not be mistaken for it\n");

    expect_split_ok("http://user:pass@host/path", false, "host", "80", "/path",
                     "user:pass");

    // A path may legitimately contain '@' - it must not be read as
    // credentials just because one turns up somewhere in the string.
    expect_split_ok("http://host/path@notauser", false, "host", "80",
                     "/path@notauser", "");
}

static void test_split_ipv6(void)
{
    printf("split: bracketed IPv6 literals, with and without a port\n");

    expect_split_ok("http://[::1]/path", false, "::1", "80", "/path", "");
    expect_split_ok("https://[2001:db8::1]:8443/path", true, "2001:db8::1",
                     "8443", "/path", "");
}

static void test_split_path_and_query(void)
{
    printf("split: missing path, bare query, and a fragment being dropped\n");

    expect_split_ok("http://host", false, "host", "80", "/", "");
    expect_split_ok("http://host?a=1", false, "host", "80", "/?a=1", "");
    expect_split_ok("http://host/path#frag", false, "host", "80", "/path", "");

    // A station address with no path at all is ordinary in the directory. The
    // '/' has to be invented here, because the request line is written straight
    // from this field: an empty one gives "GET  HTTP/1.1", which is a 400.
    expect_split_ok("http://host:8000", false, "host", "8000", "/", "");
    expect_split_ok("https://host:8443", true, "host", "8443", "/", "");
    expect_split_ok("http://user:pass@host:8000", false, "host", "8000", "/",
                     "user:pass");
    expect_split_ok("http://host#frag", false, "host", "80", "/", "");
    expect_split_ok("http://host:8000/", false, "host", "8000", "/", "");
}

// ---------------------------------------------------------------------------
// Percent-encoding of the path
//
// The path field is copied onto the request line verbatim, so anything in it
// that a request target may not contain is a malformed request - a 400 the
// station is blamed for. Directory data is not a trusted source of well-formed
// URLs, so the encoding has to happen here rather than being assumed.

static void expect_path(const char *in, const char *want_path)
{
    SwUrlParts p;
    memset(&p, '#', sizeof(p));
    bool ok = swUrlSplit(in, &p);

    CHECK(ok, "%s should have split", in);
    if (ok) CHECK(strcmp(p.path, want_path) == 0,
                  "%s: path \"%s\", wanted \"%s\"", in, p.path, want_path);
}

static void test_split_path_encoding(void)
{
    printf("split: a path is percent-encoded to what a request target may hold\n");

    // A raw space ends the request target early: the server reads "mount" as
    // the HTTP version and answers 400.
    expect_path("http://host/my mount", "/my%20mount");
    expect_path("http://host/a b?c d=e f", "/a%20b?c%20d=e%20f");

    // Non-ASCII. Station names with accents reach the path as raw UTF-8.
    expect_path("http://host/caf\xC3\xA9", "/caf%C3%A9");

    // Characters RFC 3986 excludes outright, which strict servers reject.
    expect_path("http://host/a|b", "/a%7Cb");
    expect_path("http://host/a\\b", "/a%5Cb");
    expect_path("http://host/a\"b", "/a%22b");
    expect_path("http://host/a<b>c", "/a%3Cb%3Ec");
    expect_path("http://host/a{b}c", "/a%7Bb%7Dc");
    expect_path("http://host/a^b`c", "/a%5Eb%60c");

    // The one that is not merely a 400. A CR or LF ends the request *line*, so
    // a directory entry containing one appends headers of its own choosing to
    // the app's request.
    expect_path("http://host/live\r\nX-Evil: 1", "/live%0D%0AX-Evil:%201");
    expect_path("http://host/a\nb", "/a%0Ab");
    expect_path("http://host/a\tb", "/a%09b");
    expect_path("http://host/a\x7F" "b", "/a%7Fb");
}

static void test_split_path_encoding_is_idempotent(void)
{
    printf("split: an existing %%XX escape is passed through, never re-encoded\n");

    // Re-encoding "%20" into "%2520" would be a different path, and the second
    // pass is not hypothetical: every redirect hop re-splits the URL it built.
    expect_path("http://host/my%20mount", "/my%20mount");
    expect_path("http://host/a%2Fb", "/a%2Fb");
    expect_path("http://host/a%7cb", "/a%7cb");     // case is preserved as sent

    // A real zeno.fm token. Losing or altering any of it gives a 403.
    expect_path("http://stream-174.zeno.fm/q97eczydqrhvv?zt=eyJhbGciOiJIUzI1NiJ9.abc",
                 "/q97eczydqrhvv?zt=eyJhbGciOiJIUzI1NiJ9.abc");

    // A '%' that is not a valid escape is itself unsafe and gets encoded, so
    // what comes out can always be decoded again.
    expect_path("http://host/100%", "/100%25");
    expect_path("http://host/a%zzb", "/a%25zzb");
    expect_path("http://host/a%2", "/a%252");

    // Running the output back through must not change it a second time.
    const char *urls[] = {
        "http://host/my mount", "http://host/caf\xC3\xA9",
        "http://host/100%", "http://host/live\r\nX-Evil: 1",
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        SwUrlParts once, twice;
        char rebuilt[SW_URL_MAX + 64];
        if (!swUrlSplit(urls[i], &once)) { CHECK(false, "%s split", urls[i]); continue; }
        snprintf(rebuilt, sizeof(rebuilt), "http://host%s", once.path);
        if (!swUrlSplit(rebuilt, &twice)) { CHECK(false, "%s re-split", rebuilt); continue; }
        CHECK(strcmp(once.path, twice.path) == 0,
              "re-splitting changed \"%s\" into \"%s\"", once.path, twice.path);
    }
}

static void test_split_path_encoding_preserves_ordinary_urls(void)
{
    printf("split: an ordinary path is left exactly alone\n");

    // The encoder must not "fix" addresses that already worked - every one of
    // these characters is legal in a request target.
    expect_path("http://host/Heart80sMP3", "/Heart80sMP3");
    expect_path("http://host/stream.mp3", "/stream.mp3");
    expect_path("http://host/a/b/c", "/a/b/c");
    expect_path("http://host/m?a=1&b=2", "/m?a=1&b=2");
    expect_path("http://host/path@notauser", "/path@notauser");
    expect_path("http://host/a-b_c~d.e", "/a-b_c~d.e");
    expect_path("http://host/a!b$c&d'e(f)g*h+i,j;k=l", "/a!b$c&d'e(f)g*h+i,j;k=l");
    expect_path("http://host/a:b", "/a:b");
    expect_path("http://host/live?x[]=1", "/live?x[]=1");
}

static void test_split_path_encoding_capacity(void)
{
    printf("split: a path that will not fit once encoded is refused, not truncated\n");

    // Encoding triples the worst case, so a path that fitted raw can stop
    // fitting. It must refuse - a truncated path is a request for the wrong
    // mount that still looks like it succeeded.
    static char url[SW_URL_MAX + 64];
    char spaces[900];
    memset(spaces, ' ', sizeof(spaces) - 1);
    spaces[sizeof(spaces) - 1] = 0;
    snprintf(url, sizeof(url), "http://host/%s", spaces);   // 899 * 3 = 2697 > 2047
    expect_split_fail(url);

    // Just inside the limit still works, so the refusal above is the length
    // doing it and not the encoder giving up on spaces in general.
    char ok_spaces[600];
    memset(ok_spaces, ' ', sizeof(ok_spaces) - 1);
    ok_spaces[sizeof(ok_spaces) - 1] = 0;
    snprintf(url, sizeof(url), "http://host/%s", ok_spaces); // 599 * 3 = 1797
    SwUrlParts p;
    CHECK(swUrlSplit(url, &p), "a 1797-byte encoded path should still fit");
    CHECK(strlen(p.path) == 1 + 599 * 3, "encoded length was %zu, wanted %d",
          strlen(p.path), 1 + 599 * 3);
}

static void test_split_rejections(void)
{
    printf("split: empty host, a bare colon, and a non-numeric port are all refused\n");

    expect_split_fail("http://");
    expect_split_fail("http:///path");
    expect_split_fail("http://:8000/path");
    expect_split_fail("http://host:/path");
    expect_split_fail("http://host:abc/path");

    // Out of range, not merely non-numeric. These were accepted before the
    // range check went in, and for a literal-IP host the port string never
    // reaches getaddrinfo() - tcp.c's numeric_addrinfo does
    // htons((uint16_t)atoi(port)) instead, and that cast wraps silently. So
    // "1.2.3.4:99999" connected to port 34463: a real connection to an address
    // no station ever published. Refusing is the only answer that does not
    // invent a destination.
    expect_split_fail("http://1.2.3.4:99999/stream");
    expect_split_fail("http://host:65536/path");     // one past the top
    expect_split_fail("http://host:4294967296/x");   // wraps a 32-bit accumulator
    expect_split_fail("http://host:0/path");         // port 0 is not connectable
    expect_split_fail("http://host:00000/path");     // and neither is a padded 0

    // The controls. Without these the checks above would also pass against a
    // swUrlSplit that had simply started refusing every port it was given.
    expect_split_ok("http://host:65535/path", false, "host", "65535", "/path", "");
    expect_split_ok("http://host:1/path",     false, "host", "1",     "/path", "");
}

static void test_split_capacity(void)
{
    printf("split: a host or path that will not fit is refused, never truncated\n");

    // A truncated host is a connection to the wrong place, which is exactly
    // what SW_URL_HOST_MAX (256) exists to prevent.
    static char long_host_url[512];
    char long_host[400];
    memset(long_host, 'a', sizeof(long_host) - 1);
    long_host[sizeof(long_host) - 1] = 0;
    snprintf(long_host_url, sizeof(long_host_url), "http://%s/x", long_host);
    expect_split_fail(long_host_url);

    static char long_path_url[2600];
    char long_path[2200];
    memset(long_path, 'x', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = 0;
    snprintf(long_path_url, sizeof(long_path_url), "http://host/%s", long_path);
    expect_split_fail(long_path_url);
}

// ---------------------------------------------------------------------------
// swUrlBase64
//
// Feeds the Authorization header for stations that need it. The three padding
// lengths are the classic off-by-one trap in any base64 encoder, so all three
// get a known vector rather than trusting one "it compiled" case.

static void expect_b64(const char *in, const char *want)
{
    char out[64];
    memset(out, '#', sizeof(out));
    bool ok = swUrlBase64(in, out, sizeof(out));

    CHECK(ok, "%s should have encoded", in);
    if (ok) CHECK(strcmp(out, want) == 0, "%s -> \"%s\", wanted \"%s\"",
                  in, out, want);
}

static void test_base64_known_vectors(void)
{
    printf("base64: known vectors covering all three padding lengths\n");

    expect_b64("a", "YQ==");
    expect_b64("ab", "YWI=");
    expect_b64("abc", "YWJj");
    expect_b64("user:pass", "dXNlcjpwYXNz");
    expect_b64("", "");
}

static void test_base64_capacity(void)
{
    printf("base64: a buffer one byte too small is refused\n");

    // "abc" -> "YWJj" needs 4 chars + terminator = 5 bytes.
    char out[4];
    memset(out, '#', sizeof(out));
    CHECK(!swUrlBase64("abc", out, sizeof(out)),
          "one byte short must be refused");
}

// ---------------------------------------------------------------------------
// swUrlBasicAuth
//
// This is the 401 the owner reported. swHttpBuildGet writes the "Basic " itself,
// and http.c used to write one too, so the header on the wire read
// `Authorization: Basic Basic dXNlcjpwYXNz` - unparseable, and every station
// whose directory entry carries credentials answered 401 (or 400, on the
// stricter servers). http.c is 3DS-only and no host suite compiles it, so the
// rule lives here where it can actually be held to.

static void expect_auth(const char *userinfo, const char *want)
{
    char out[256];
    memset(out, '#', sizeof(out));
    bool ok = swUrlBasicAuth(userinfo, out, sizeof(out));

    if (!want) {
        CHECK(!ok, "\"%s\" should have been refused, got \"%s\"",
              userinfo, ok ? out : "");
        return;
    }
    CHECK(ok, "\"%s\" should have encoded", userinfo);
    if (ok) CHECK(strcmp(out, want) == 0, "\"%s\" -> \"%s\", wanted \"%s\"",
                  userinfo, out, want);
}

static void test_basic_auth_has_no_scheme_prefix(void)
{
    printf("basicauth: the value is bare base64 - the \"Basic \" is the builder's job\n");

    expect_auth("user:pass", "dXNlcjpwYXNz");

    // The regression guard proper. swHttpBuildGet emits
    // "Authorization: Basic %s", so anything scheme-shaped coming back from
    // here is the doubled header that caused the bug.
    char out[256];
    CHECK(swUrlBasicAuth("user:pass", out, sizeof(out)), "should encode");
    CHECK(strncmp(out, "Basic", 5) != 0,
          "the value must not carry a scheme token, got \"%s\"", out);
    CHECK(strchr(out, ' ') == NULL,
          "base64 contains no spaces; \"%s\" means a prefix crept back in", out);

    // Known vectors, so this cannot pass by returning something merely
    // prefix-free.
    expect_auth("aladdin:opensesame", "YWxhZGRpbjpvcGVuc2VzYW1l");
    expect_auth("a:b", "YTpi");
}

static void test_basic_auth_decodes_escapes(void)
{
    printf("basicauth: percent-escapes in the userinfo are decoded before encoding\n");

    // A password containing '@', ':' or '/' can only appear in a URL escaped.
    // Encoding the escaped form sends a password the station never issued.
    expect_auth("user:p%40ss", "dXNlcjpwQHNz");           // "user:p@ss"
    expect_auth("user:p%2Fss", "dXNlcjpwL3Nz");           // "user:p/ss"
    expect_auth("us%65r:pass", "dXNlcjpwYXNz");           // "user:pass"
    expect_auth("user:p%20ss", "dXNlcjpwIHNz");           // "user:p ss"

    // Lower-case hex is just as valid.
    expect_auth("user:p%40ss", "dXNlcjpwQHNz");
    expect_auth("user:p%4055", "dXNlcjpwQDU1");

    // A '%' that is not a valid escape is a literal '%', not a decode failure -
    // refusing would turn a working station into a dead one.
    expect_auth("user:100%", "dXNlcjoxMDAl");             // "user:100%"
    expect_auth("user:a%zz", "dXNlcjphJXp6");             // "user:a%zz"
    expect_auth("user:a%2", "dXNlcjphJTI=");              // "user:a%2"

    // A decoded NUL would silently truncate the credentials.
    expect_auth("user:a%00b", NULL);

    // No credentials at all is not an error, it is an empty string - http.c
    // tests userinfo[0] before it ever calls this.
    expect_auth("", "");
}

static void test_basic_auth_capacity(void)
{
    printf("basicauth: a result that will not fit is refused\n");

    // "user:pass" -> "dXNlcjpwYXNz", 12 chars + terminator = 13.
    char exact[13];
    CHECK(swUrlBasicAuth("user:pass", exact, sizeof(exact)),
          "exactly enough room should succeed");
    CHECK(strcmp(exact, "dXNlcjpwYXNz") == 0, "got \"%s\"", exact);

    char tight[12];
    memset(tight, '#', sizeof(tight));
    CHECK(!swUrlBasicAuth("user:pass", tight, sizeof(tight)),
          "one byte short must be refused");

    // Longer than SW_URL_AUTH_MAX once decoded - refused, never truncated.
    char huge[SW_URL_AUTH_MAX * 3];
    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    char out[512];
    CHECK(!swUrlBasicAuth(huge, out, sizeof(out)),
          "an over-long userinfo must be refused");

    CHECK(!swUrlBasicAuth(NULL, out, sizeof(out)), "NULL input must be refused");
}

static void test_basic_auth_matches_a_split_url(void)
{
    printf("basicauth: the whole path from a real credentialed station URL\n");

    // End to end: exactly what http.c does with a directory entry that carries
    // credentials, minus the socket.
    SwUrlParts p;
    CHECK(swUrlSplit("http://user:pass@stream.example.com:8000/mount", &p),
          "a credentialed station URL should split");
    CHECK(strcmp(p.userinfo, "user:pass") == 0, "userinfo \"%s\"", p.userinfo);

    char value[256];
    CHECK(swUrlBasicAuth(p.userinfo, value, sizeof(value)), "should encode");

    // The header swHttpBuildGet will now produce from this.
    char header[320];
    snprintf(header, sizeof(header), "Authorization: Basic %s", value);
    CHECK(strcmp(header, "Authorization: Basic dXNlcjpwYXNz") == 0,
          "header was \"%s\"", header);
    CHECK(strstr(header, "Basic Basic") == NULL,
          "the doubled scheme token is back: \"%s\"", header);
}

// ---------------------------------------------------------------------------
// swUrlHostEq
//
// Decides whether credentials may follow a redirect. A false positive sends a
// station's password to somebody else's CDN, so the comparison is worth pinning
// rather than assuming strcasecmp-shaped behaviour.

static void test_host_equality(void)
{
    printf("hosteq: hostnames compare case-insensitively, and nothing else matches\n");

    CHECK(swUrlHostEq("host", "host"), "identical hosts must match");
    CHECK(swUrlHostEq("STREAM.example.com", "stream.EXAMPLE.com"),
          "hostnames are case-insensitive");
    CHECK(swUrlHostEq("", ""), "two empty hosts match");
    CHECK(swUrlHostEq("::1", "::1"), "an IPv6 literal matches itself");

    CHECK(!swUrlHostEq("host", "other"), "different hosts must not match");
    CHECK(!swUrlHostEq("host", "host.evil.com"),
          "a prefix must not match - this is the CDN leak");
    CHECK(!swUrlHostEq("host.evil.com", "host"),
          "nor the other way round");
    CHECK(!swUrlHostEq("host", "hosts"), "one trailing character is a difference");
    CHECK(!swUrlHostEq("host", ""), "empty matches nothing but empty");
    CHECK(!swUrlHostEq("", "host"), "nor in the other direction");
    CHECK(!swUrlHostEq(NULL, "host"), "NULL must not match");
    CHECK(!swUrlHostEq("host", NULL), "NULL must not match either way");
}

static void test_host_equality_gates_redirect_credentials(void)
{
    printf("hosteq: the same-host rule as http.c applies it across a redirect\n");

    // A mount redirect back to the same host keeps the credentials; this is the
    // Icecast case, and dropping them is a 401 one hop later.
    SwUrlParts origin, same, other;
    CHECK(swUrlSplit("http://user:pass@stream.example.com:8000/mount", &origin),
          "origin should split");

    char next[SW_URL_MAX];
    CHECK(swUrlResolve(&origin, "/mount2", next, sizeof(next)),
          "a bare-path Location should resolve");
    CHECK(swUrlSplit(next, &same), "the resolved URL should split");

    // swUrlResolve does not carry userinfo, which is exactly why http.c has to.
    CHECK(same.userinfo[0] == 0,
          "resolve must not smuggle credentials into the URL, got \"%s\"",
          same.userinfo);
    CHECK(swUrlHostEq(same.host, origin.host),
          "a bare-path redirect stays on the same host, so credentials follow");

    // A redirect to a CDN must not be handed the origin's password.
    CHECK(swUrlSplit("http://cdn.other.net/mount", &other), "cdn should split");
    CHECK(!swUrlHostEq(other.host, origin.host),
          "a different host must not inherit credentials");
}

// ---------------------------------------------------------------------------
// swUrlResolve
//
// Turns a Location header into the next URL to open. Icecast mount redirects
// are frequently bare paths, so the relative and protocol-relative forms are
// exercised as heavily as the trivial "already absolute" case.

static SwUrlParts mk_base(bool tls, const char *host, const char *port,
                           const char *path)
{
    SwUrlParts b;
    memset(&b, 0, sizeof(b));
    b.tls = tls;
    snprintf(b.host, sizeof(b.host), "%s", host);
    snprintf(b.port, sizeof(b.port), "%s", port);
    snprintf(b.path, sizeof(b.path), "%s", path);
    return b;
}

static void expect_resolve(const SwUrlParts *base, const char *loc,
                            const char *want)
{
    char out[256];
    memset(out, '#', sizeof(out));
    bool ok = swUrlResolve(base, loc, out, sizeof(out));

    if (!want) {
        CHECK(!ok, "\"%s\" should have been refused, got \"%s\"",
              loc, ok ? out : "");
        return;
    }
    CHECK(ok, "\"%s\" should have resolved", loc);
    if (ok) CHECK(strcmp(out, want) == 0, "\"%s\" -> \"%s\", wanted \"%s\"",
                  loc, out, want);
}

static void test_resolve_absolute_and_protocol_relative(void)
{
    printf("resolve: an absolute Location passes through, protocol-relative keeps the base scheme\n");

    SwUrlParts base = mk_base(true, "orig.host", "443", "/mount");

    expect_resolve(&base, "http://other/y", "http://other/y");
    expect_resolve(&base, "https://other/y", "https://other/y");

    expect_resolve(&base, "//other.example/z", "https://other.example/z");

    SwUrlParts plain_base = mk_base(false, "orig.host", "80", "/mount");
    expect_resolve(&plain_base, "//other.example/z", "http://other.example/z");
}

static void test_resolve_paths(void)
{
    printf("resolve: absolute path, relative path, and a root base path\n");

    SwUrlParts base = mk_base(false, "host", "80", "/mount/old");

    expect_resolve(&base, "/stream2", "http://host/stream2");
    expect_resolve(&base, "stream2", "http://host/mount/stream2");

    SwUrlParts root_base = mk_base(false, "host", "80", "/");
    expect_resolve(&root_base, "stream2", "http://host/stream2");
}

static void test_resolve_ports(void)
{
    printf("resolve: a non-default base port survives, a default port is omitted\n");

    SwUrlParts custom_port = mk_base(false, "host", "8000", "/mount/old");
    expect_resolve(&custom_port, "y", "http://host:8000/mount/y");

    SwUrlParts default_port = mk_base(false, "host", "80", "/mount/old");
    expect_resolve(&default_port, "/z", "http://host/z");

    SwUrlParts default_tls_port = mk_base(true, "host", "443", "/mount/old");
    expect_resolve(&default_tls_port, "/z", "https://host/z");
}

static void test_resolve_ipv6(void)
{
    printf("resolve: an IPv6 host gets its brackets back\n");

    // SwUrlParts.host holds the literal with the brackets stripped, which is
    // what a socket call wants. A URL is not a socket call: without them,
    // "http://::1:8080/x" gives no way to tell where the address ends and the
    // port begins, and the redirect that produced it goes nowhere.
    SwUrlParts v6 = mk_base(false, "::1", "8080", "/mount/old");
    expect_resolve(&v6, "stream2", "http://[::1]:8080/mount/stream2");
    expect_resolve(&v6, "/stream2", "http://[::1]:8080/stream2");

    SwUrlParts v6_default = mk_base(false, "2001:db8::1", "80", "/mount/old");
    expect_resolve(&v6_default, "/z", "http://[2001:db8::1]/z");

    SwUrlParts v6_tls = mk_base(true, "2001:db8::1", "443", "/mount/old");
    expect_resolve(&v6_tls, "/z", "https://[2001:db8::1]/z");

    // The reason the brackets matter at all: what comes out has to survive
    // being parsed again, which is exactly what the redirect loop does with it.
    char out[256];
    SwUrlParts again;
    if (swUrlResolve(&v6, "stream2", out, sizeof(out)) &&
        swUrlSplit(out, &again)) {
        CHECK(strcmp(again.host, "::1") == 0,
              "re-parsed host was \"%s\", wanted \"::1\"", again.host);
        CHECK(strcmp(again.port, "8080") == 0,
              "re-parsed port was \"%s\", wanted \"8080\"", again.port);
    } else {
        CHECK(false, "a resolved IPv6 URL must parse back into the same parts");
    }
}

static void test_resolve_rejections(void)
{
    printf("resolve: an empty Location, and a result too long for the buffer, are both refused\n");

    SwUrlParts base = mk_base(false, "host", "80", "/mount/old");
    expect_resolve(&base, "", NULL);

    char tiny[5];
    memset(tiny, '#', sizeof(tiny));
    CHECK(!swUrlResolve(&base, "y", tiny, sizeof(tiny)),
          "a result longer than the buffer must be refused");
}

int main(void)
{
    printf("== url ==\n");
    test_real_station_urls();
    test_parts_are_preserved();
    test_scheme_matching();
    test_capacity();

    test_split_schemes_and_ports();
    test_split_userinfo();
    test_split_ipv6();
    test_split_path_and_query();
    test_split_path_encoding();
    test_split_path_encoding_is_idempotent();
    test_split_path_encoding_preserves_ordinary_urls();
    test_split_path_encoding_capacity();
    test_split_rejections();
    test_split_capacity();

    test_base64_known_vectors();
    test_base64_capacity();

    test_basic_auth_has_no_scheme_prefix();
    test_basic_auth_decodes_escapes();
    test_basic_auth_capacity();
    test_basic_auth_matches_a_split_url();

    test_host_equality();
    test_host_equality_gates_redirect_credentials();

    test_resolve_absolute_and_protocol_relative();
    test_resolve_paths();
    test_resolve_ports();
    test_resolve_ipv6();
    test_resolve_rejections();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
