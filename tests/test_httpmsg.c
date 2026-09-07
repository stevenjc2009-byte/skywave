// Host tests for HTTP message framing: response-head parsing, header lookup,
// request building, and chunked-transfer decoding.
//
// httpmsg.c is brand new - it replaces libctru's httpc, so a parser bug here
// is a bug the app now owns that it never used to. Nothing in this file talks
// to a socket; every case is built as a plain buffer so the whole thing runs
// in a fraction of a second on a PC, same as test_url.c.
//
// Build: see tests/Makefile.

#include "../source/net/httpmsg.h"
#include "../source/net/url.h"   // SW_URL_HOST_MAX / SwUrlParts::port - the hostline bound

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

// ------------------------------------------------------------- head parsing

static void test_status_lines(void)
{
    printf("status line shapes: HTTP/1.1, HTTP/1.0, and Shoutcast's ICY\n");

    {
        // The trailing "X" is one byte of body, the ordinary case where a
        // read brings the head and the start of the body together. The head
        // arriving on its own is covered by
        // test_crlf_terminator_at_buffer_end() below.
        const char *msg = "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_OK, "expected OK, got %d", st);
        CHECK(h.status == 200, "status %d, want 200", h.status);
        CHECK(!h.icy, "HTTP/1.1 response must not be flagged icy");
    }
    {
        const char *msg = "HTTP/1.0 301 Moved\r\nLocation: http://x/y\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_OK, "expected OK, got %d", st);
        CHECK(h.status == 301, "status %d, want 301", h.status);
        CHECK(!h.icy, "HTTP/1.0 response must not be flagged icy");
    }
    {
        // Shoutcast v1: no HTTP/x.y token at all. Rejecting this turns a
        // working station into a dead one, so it must parse and must set icy.
        const char *msg = "ICY 200 OK\r\nicy-name: Test Radio\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_OK, "expected OK, got %d", st);
        CHECK(h.status == 200, "status %d, want 200", h.status);
        CHECK(h.icy, "ICY status line must set .icy true");
    }
    {
        // The FAILING Shoutcast statuses, which is where .icy actually earns
        // its keep. swHttpErrorText branches on it to say something different
        // from the HTTP table, because Shoutcast's numbers are its own
        // vocabulary and collide with HTTP's on the two codes users report:
        // ICY 401 means the mount is not broadcasting, not "unauthorized", and
        // ICY 400 means the server is refusing the listener, not "malformed
        // request". Both halves have to hold for that branch to be reachable -
        // .icy true AND the number parsed - so both are checked here. Only
        // ICY 200 was covered before, so the error path this relies on had
        // never been exercised at all.
        static const int codes[] = { 400, 401, 403, 404, 502 };
        for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
            char msg[64];
            snprintf(msg, sizeof(msg), "ICY %d Service Unavailable\r\n\r\nX", codes[i]);

            SwHttpHead h;
            size_t body_off;
            SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
            CHECK(st == SW_HEAD_OK, "ICY %d: expected OK, got %d", codes[i], st);
            CHECK(h.status == codes[i], "ICY %d: status %d", codes[i], h.status);
            CHECK(h.icy, "ICY %d must set .icy true", codes[i]);
        }
    }
}

static void test_line_endings(void)
{
    printf("CRLF and bare-LF header termination both work\n");

    {
        const char *msg = "HTTP/1.1 200 OK\r\nX-A: 1\r\n\r\nBODY";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_OK, "CRLF head: expected OK, got %d", st);
        CHECK(body_off == strlen("HTTP/1.1 200 OK\r\nX-A: 1\r\n\r\n"),
              "CRLF head: body_off %zu wrong", body_off);
    }
    {
        // A server that never learned about CR. find_head_end() explicitly
        // supports this (see its comment); it must not be treated as garbage.
        const char *msg = "HTTP/1.1 200 OK\nX-A: 1\n\nBODY";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_OK, "bare-LF head: expected OK, got %d", st);
        CHECK(body_off == strlen("HTTP/1.1 200 OK\nX-A: 1\n\n"),
              "bare-LF head: body_off %zu wrong", body_off);
    }
}

static void test_incremental_feed(void)
{
    printf("no blank line yet needs more; the full buffer then parses\n");

    // Trailing "X" is one byte of body; the head-only case is
    // test_crlf_terminator_at_buffer_end() below.
    const char *full = "HTTP/1.1 200 OK\r\nX-A: 1\r\n\r\nX";
    const char *partial = "HTTP/1.1 200 OK\r\nX-A: 1\r\n"; // no blank line yet

    SwHttpHead h;
    size_t body_off;

    SwHeadState st = swHttpHeadParse(partial, strlen(partial), &h, &body_off);
    CHECK(st == SW_HEAD_NEED_MORE, "no blank line yet: expected NEED_MORE, got %d", st);

    st = swHttpHeadParse(full, strlen(full), &h, &body_off);
    CHECK(st == SW_HEAD_OK, "complete buffer: expected OK, got %d", st);
}

static void test_split_mid_header_line(void)
{
    printf("a header line split across two feeds still parses once complete\n");

    // Trailing "X" is one byte of body; the head-only case is
    // test_crlf_terminator_at_buffer_end() below.
    const char *full = "HTTP/1.1 200 OK\r\nX-Custom-Header: somevalue\r\n\r\nX";
    const char *cut_after_value_prefix = "HTTP/1.1 200 OK\r\nX-Custom-Header: some";
    size_t split_at = strlen(cut_after_value_prefix);

    SwHttpHead h;
    size_t body_off;

    SwHeadState st = swHttpHeadParse(full, split_at, &h, &body_off);
    CHECK(st == SW_HEAD_NEED_MORE,
          "buffer cut mid-header-line: expected NEED_MORE, got %d", st);

    st = swHttpHeadParse(full, strlen(full), &h, &body_off);
    CHECK(st == SW_HEAD_OK, "full buffer: expected OK, got %d", st);
    if (st == SW_HEAD_OK) {
        char v[64];
        CHECK(swHttpHeadValue(&h, "X-Custom-Header", v, sizeof(v)) &&
              strcmp(v, "somevalue") == 0,
              "header value came through wrong across the split, got \"%s\"", v);
    }
}

static void test_body_off_with_body_present(void)
{
    printf("body_off points at the first real body byte when it is already buffered\n");

    const char *head_part = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
    const char *body_part = "helloEXTRADATA";
    char buf[256];
    int n = snprintf(buf, sizeof(buf), "%s%s", head_part, body_part);
    (void)n;

    SwHttpHead h;
    size_t body_off = (size_t)-1;
    SwHeadState st = swHttpHeadParse(buf, strlen(buf), &h, &body_off);
    CHECK(st == SW_HEAD_OK, "expected OK, got %d", st);
    CHECK(body_off == strlen(head_part), "body_off %zu, want %zu",
          body_off, strlen(head_part));
    CHECK(strcmp(buf + body_off, body_part) == 0,
          "bytes at body_off are \"%s\", want \"%s\"", buf + body_off, body_part);
}

static void test_rejects_bad_status_lines(void)
{
    printf("a status line that is not HTTP/x.y or ICY is refused\n");

    {
        // Trailing "X" is one byte of body, so this reaches the status-line
        // check with a head find_head_end() has definitely completed - a test
        // that stopped short of the check would pass for the wrong reason.
        const char *msg = "FOO/1.1 200 OK\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_BAD, "non-HTTP status line: expected BAD, got %d", st);
    }
}

static void test_rejects_wrong_digit_counts(void)
{
    printf("a status code that is not exactly three digits is refused\n");

    {
        // Two digits: the digit loop stops at the space with digits == 2.
        const char *msg = "HTTP/1.1 20 OK\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_BAD, "2-digit code: expected BAD, got %d", st);
    }
    {
        // Four digits. Per the header comment in httpmsg.h ("An HTTP status
        // is three digits by definition. Anything else means this is not a
        // response head"), this must be refused.
        //
        // Regression: this used to parse as a perfectly good 200. The digit
        // loop is `while (... && digits < 3)`, so it stopped after three
        // digits and never looked at the character following them - the
        // trailing '0' of "2000" went entirely unexamined and a malformed
        // status line was accepted as a truncated valid one. For the updater
        // that is a response head it was about to install from, so
        // swHttpHeadParse() now checks for that fourth digit explicitly.
        const char *msg = "HTTP/1.1 2000 OK\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_BAD,
              "4-digit status \"2000\": expected BAD, got st=%d status=%d",
              st, h.status);
    }
}

static void test_rejects_out_of_range_codes(void)
{
    printf("a three-digit code outside 100..599 is refused\n");

    {
        const char *msg = "HTTP/1.1 050 Weird\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_BAD, "050 (< 100): expected BAD, got %d", st);
    }
    {
        const char *msg = "HTTP/1.1 700 Weird\r\n\r\nX";
        SwHttpHead h;
        size_t body_off;
        SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
        CHECK(st == SW_HEAD_BAD, "700 (> 599): expected BAD, got %d", st);
    }
}

static void test_head_max_size(void)
{
    printf("a head bigger than SW_HEAD_MAX is refused, not left hanging forever\n");

    static char buf[SW_HEAD_MAX + 512];
    size_t n = 0;
    n += (size_t)snprintf(buf + n, sizeof(buf) - n, "HTTP/1.1 200 OK\r\n");
    // Never emit a blank line - this must stay a header block with no
    // terminator all the way past SW_HEAD_MAX.
    while (n < SW_HEAD_MAX + 100) {
        n += (size_t)snprintf(buf + n, sizeof(buf) - n,
                              "X-Filler: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n");
    }

    SwHttpHead h;
    size_t body_off;
    SwHeadState st = swHttpHeadParse(buf, n, &h, &body_off);
    CHECK(st == SW_HEAD_BAD, "oversized, unterminated head: expected BAD, got %d", st);
}

static void test_crlf_terminator_at_buffer_end(void)
{
    printf("a CRLF terminator landing exactly at buffer end completes the head\n");

    // Regression. find_head_end()'s CRLF-blank-line branch used to read
    //     if (i + 3 < len && buf[i+1]=='\r' && buf[i+2]=='\n') return i + 3;
    // which demands one byte MORE than the terminator itself - i+3 is the
    // offset one past it, so it only needs to be <= len. A head ending in
    // "\r\n\r\n" with no body byte yet in the same read came back as
    // SW_HEAD_NEED_MORE instead of SW_HEAD_OK. Usually it self-healed on the
    // next read, but against a server that sends a head and then nothing it
    // waited for a byte that was never coming. The bare-LF branch never had
    // the fault, which is why only this shape needs the check.
    const char *msg = "HTTP/1.1 200 OK\r\n\r\n"; // exactly the head, nothing more
    SwHttpHead h;
    size_t body_off = (size_t)-1;
    SwHeadState st = swHttpHeadParse(msg, strlen(msg), &h, &body_off);
    CHECK(st == SW_HEAD_OK,
          "head with no trailing body byte: expected OK, got %d", st);
    CHECK(st == SW_HEAD_OK && h.status == 200, "status %d, want 200", h.status);
    CHECK(body_off == strlen(msg),
          "body_off %zu should point one past the head (%zu)",
          body_off, strlen(msg));

    // The same shape with headers in it, since the terminator search walks
    // past a header block rather than starting on the blank line.
    const char *msg2 = "HTTP/1.1 302 Found\r\nLocation: http://x/y\r\n\r\n";
    SwHttpHead h2;
    size_t body_off2 = (size_t)-1;
    SwHeadState st2 = swHttpHeadParse(msg2, strlen(msg2), &h2, &body_off2);
    CHECK(st2 == SW_HEAD_OK, "headers + bare terminator: expected OK, got %d", st2);
    if (st2 == SW_HEAD_OK) {
        char v[64];
        CHECK(swHttpHeadValue(&h2, "Location", v, sizeof(v)) &&
              strcmp(v, "http://x/y") == 0,
              "Location came through as \"%s\"", v);
    }
}

// ------------------------------------------------------------- header value

static void make_head(SwHttpHead *h, const char *raw_lines)
{
    memset(h, 0, sizeof(*h));
    size_t n = strlen(raw_lines);
    if (n >= sizeof(h->raw)) n = sizeof(h->raw) - 1;
    memcpy(h->raw, raw_lines, n);
    h->raw[n] = 0;
    h->raw_len = n;
}

static void test_head_value(void)
{
    printf("header lookup: case-insensitive, trimmed, absent, and too-long-for-buffer\n");

    SwHttpHead h;
    make_head(&h,
              "Icy-MetaInt: 8192\r\n"
              "X-Padded:    hello world   \r\n"
              "X-Long-Value: 0123456789ABCDEF\r\n");

    {
        char v[32];
        // The header arrives as "Icy-MetaInt"; look it up by a different case
        // entirely, the way icy-metaint really does get asked for.
        CHECK(swHttpHeadValue(&h, "icy-metaint", v, sizeof(v)) &&
              strcmp(v, "8192") == 0,
              "case-insensitive lookup failed, got \"%s\"", v);
    }
    {
        char v[32];
        CHECK(swHttpHeadValue(&h, "X-Padded", v, sizeof(v)) &&
              strcmp(v, "hello world") == 0,
              "whitespace not trimmed, got \"%s\"", v);
    }
    {
        char v[32];
        CHECK(!swHttpHeadValue(&h, "X-Not-There", v, sizeof(v)),
              "absent header must return false");
    }
    {
        // Value is 16 bytes ("0123456789ABCDEF"); a 8-byte buffer cannot
        // hold it plus the NUL, so this must fail rather than truncate.
        char v[8];
        v[0] = '#';
        CHECK(!swHttpHeadValue(&h, "X-Long-Value", v, sizeof(v)),
              "value too long for buffer must return false");
        CHECK(v[0] == '#', "a refusal must not touch the output buffer");
    }
}

static void test_head_int(void)
{
    printf("swHttpHeadInt: parses digits, falls back on absent or non-numeric\n");

    SwHttpHead h;
    make_head(&h, "X-Count: 42\r\nX-Bad: abc\r\n");

    CHECK(swHttpHeadInt(&h, "X-Count", -1) == 42, "expected 42");
    CHECK(swHttpHeadInt(&h, "X-Bad", -7) == -7, "non-numeric must fall back");
    CHECK(swHttpHeadInt(&h, "X-Missing", -9) == -9, "absent must fall back");
}

static void test_head_chunked(void)
{
    printf("swHttpHeadChunked: case-insensitive, list-aware, absent is false\n");

    {
        SwHttpHead h;
        make_head(&h, "Transfer-Encoding: chunked\r\n");
        CHECK(swHttpHeadChunked(&h), "plain 'chunked' must be true");
    }
    {
        SwHttpHead h;
        make_head(&h, "Transfer-Encoding: ChUnKeD\r\n");
        CHECK(swHttpHeadChunked(&h), "mixed-case 'ChUnKeD' must be true");
    }
    {
        SwHttpHead h;
        make_head(&h, "Transfer-Encoding: gzip, chunked\r\n");
        CHECK(swHttpHeadChunked(&h), "'gzip, chunked' must be true");
    }
    {
        SwHttpHead h;
        make_head(&h, "Transfer-Encoding: gzip\r\n");
        CHECK(!swHttpHeadChunked(&h), "'gzip' alone must be false");
    }
    {
        SwHttpHead h;
        make_head(&h, "Content-Length: 5\r\n");
        CHECK(!swHttpHeadChunked(&h), "absent header must be false");
    }
}

static void test_hls_manifest_detection(void)
{
    printf("swHttpIsHlsManifest: Content-Type, #EXTM3U magic, and neither\n");

    {
        // The exact shape measured 2026-09-07 against two live BBC Radio 1
        // entries (see http.c's call site): Content-Type
        // "application/x-mpegurl" and a body starting "#EXTM3U". Either signal
        // alone would already catch this one; both agreeing is the common case.
        SwHttpHead h;
        make_head(&h, "Content-Type: application/x-mpegurl\r\n");
        const char *body = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-STREAM-INF:...\n";
        CHECK(swHttpIsHlsManifest(&h, body, strlen(body)),
              "Content-Type + #EXTM3U body must be detected as HLS");
    }
    {
        // RFC 8216's other registered spelling, checked case-insensitively -
        // a real server is free to send it in any case and with a charset
        // parameter tacked on.
        SwHttpHead h;
        make_head(&h, "Content-Type: Application/Vnd.Apple.MPEGURL; charset=utf-8\r\n");
        CHECK(swHttpIsHlsManifest(&h, "", 0),
              "application/vnd.apple.mpegurl Content-Type alone must be enough");
    }
    {
        // The header can be missing or wrong (see the comment on
        // swHttpIsHlsManifest for why it is not trusted alone) - the magic
        // bytes must catch the manifest on their own.
        SwHttpHead h;
        make_head(&h, "Content-Type: application/octet-stream\r\n");
        const char *body = "#EXTM3U\n#EXT-X-VERSION:3\n";
        CHECK(swHttpIsHlsManifest(&h, body, strlen(body)),
              "#EXTM3U body must be detected as HLS even with a wrong Content-Type");
    }
    {
        // No Content-Type header at all - some stream servers send none.
        SwHttpHead h;
        make_head(&h, "");
        const char *body = "#EXTM3U\n#EXT-X-VERSION:3\n";
        CHECK(swHttpIsHlsManifest(&h, body, strlen(body)),
              "#EXTM3U body must be detected with no Content-Type present");
    }
    {
        // The ordinary case: a real audio stream. Neither signal must fire.
        SwHttpHead h;
        make_head(&h, "Content-Type: audio/mpeg\r\nicy-name: Test Radio\r\n");
        const unsigned char mp3ish[] = { 0xFF, 0xFB, 0x90, 0x64, 0, 0, 0, 0 };
        CHECK(!swHttpIsHlsManifest(&h, (const char *)mp3ish, sizeof(mp3ish)),
              "an ordinary MP3 stream must not be flagged as HLS");
    }
    {
        // A NULL head (this app's call site always has one, but the function
        // takes it as a plain pointer) must fall back to the byte sniff alone
        // rather than crashing.
        const char *body = "#EXTM3U\n";
        CHECK(swHttpIsHlsManifest(NULL, body, strlen(body)),
              "NULL head must still catch the #EXTM3U magic");
        CHECK(!swHttpIsHlsManifest(NULL, "not a playlist", 14),
              "NULL head with no magic must not be flagged as HLS");
    }
    {
        // No bytes read yet - the head arrived with nothing after it. That is
        // not itself evidence of HLS; only a real magic match is.
        SwHttpHead h;
        make_head(&h, "Content-Type: audio/aac\r\n");
        CHECK(!swHttpIsHlsManifest(&h, "", 0),
              "an empty body with an audio Content-Type must not be flagged as HLS");
    }
    {
        // Fewer bytes than the magic itself - must not read past what is there
        // (ASan/UBSan would catch an overread; this pins the boundary case).
        SwHttpHead h;
        make_head(&h, "");
        CHECK(!swHttpIsHlsManifest(&h, "#EXTM", 5),
              "a body shorter than the magic must not be flagged as HLS");
    }
}

// ------------------------------------------------------------- request build

static void test_build_get(void)
{
    printf("swHttpBuildGet: Host/port rules, optional headers, and capacity\n");

    char out[512];
    int n;

    {
        n = swHttpBuildGet(out, sizeof(out), "example.com", NULL, "/stream",
                           NULL, NULL, false, NULL);
        const char *want =
            "GET /stream HTTP/1.1\r\n"
            "Host: example.com\r\n"
            "User-Agent: Skywave\r\n"
            "Accept: */*\r\n"
            "Connection: close\r\n"
            "\r\n";
        CHECK(n == (int)strlen(want), "default port/UA: n=%d, want %d",
              n, (int)strlen(want));
        CHECK(n >= 0 && strcmp(out, want) == 0,
              "default port/UA: got \"%s\"", out);
    }
    {
        // Default ports must be omitted from Host, in either spelling.
        n = swHttpBuildGet(out, sizeof(out), "example.com", "80", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: example.com\r\n") != NULL,
              "port 80 must not appear in Host, got \"%s\"", out);

        n = swHttpBuildGet(out, sizeof(out), "example.com", "443", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: example.com\r\n") != NULL,
              "port 443 must not appear in Host, got \"%s\"", out);
    }
    {
        // A non-default port must appear.
        n = swHttpBuildGet(out, sizeof(out), "example.com", "8000", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: example.com:8000\r\n") != NULL,
              "port 8000 must appear in Host, got \"%s\"", out);
    }
    {
        // Icy-MetaData only appears when asked.
        n = swHttpBuildGet(out, sizeof(out), "h", NULL, "/x", "UA", NULL,
                           true, NULL);
        CHECK(n >= 0 && strstr(out, "Icy-MetaData: 1\r\n") != NULL,
              "want_icy_meta=true must add Icy-MetaData, got \"%s\"", out);

        n = swHttpBuildGet(out, sizeof(out), "h", NULL, "/x", "UA", NULL,
                           false, NULL);
        CHECK(n >= 0 && strstr(out, "Icy-MetaData") == NULL,
              "want_icy_meta=false must not add Icy-MetaData, got \"%s\"", out);
    }
    {
        // Authorization only appears when given.
        n = swHttpBuildGet(out, sizeof(out), "h", NULL, "/x", "UA",
                           "dXNlcjpwYXNz", false, NULL);
        CHECK(n >= 0 && strstr(out, "Authorization: Basic dXNlcjpwYXNz\r\n") != NULL,
              "authorization given must appear, got \"%s\"", out);

        n = swHttpBuildGet(out, sizeof(out), "h", NULL, "/x", "UA",
                           NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Authorization") == NULL,
              "no authorization must not appear, got \"%s\"", out);
    }
    {
        // Too small to fit: must fail cleanly, not truncate.
        char tiny[10];
        n = swHttpBuildGet(tiny, sizeof(tiny), "example.com", NULL, "/stream",
                           NULL, NULL, false, NULL);
        CHECK(n == -1, "undersized buffer: expected -1, got %d", n);
    }
}

static void test_build_get_host_bound(void)
{
    printf("swHttpBuildGet: a max-length host builds, an over-long one is refused\n");

    // swHttpBuildGet()'s hostline buffer is sized from url.h rather than from
    // SW_HEAD_MAX, because it only ever holds `host` or `host:port`:
    //   host  - SW_URL_HOST_MAX (256) bytes including the NUL -> 255 chars max
    //   port  - SwUrlParts::port (8) bytes including the NUL  ->   7 chars max
    // so the worst case is 255 + ':' + 7 + NUL = 264 bytes. These checks pin
    // both ends of that: the largest input the bound promises to accept must
    // still come out whole, and one character past it must be refused rather
    // than silently truncated - a truncated Host is a well-formed request sent
    // to the wrong virtual host.
    const size_t host_max  = SW_URL_HOST_MAX - 1;
    const size_t port_max  = sizeof(((SwUrlParts *)0)->port) - 1;

    char host[SW_URL_HOST_MAX + 8];
    char want[SW_URL_HOST_MAX + 32];
    char out[1024];
    int n;

    memset(host, 'a', host_max);
    host[host_max] = 0;
    CHECK(strlen(host) == host_max, "test setup: host is %zu chars, want %zu",
          strlen(host), host_max);

    {
        // The longest host the bound allows, with no port.
        n = swHttpBuildGet(out, sizeof(out), host, NULL, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n > 0, "max-length host: expected a built request, got %d", n);

        snprintf(want, sizeof(want), "Host: %s\r\n", host);
        CHECK(n > 0 && strstr(out, want) != NULL,
              "max-length host must reach Host untruncated");
    }
    {
        // The exact worst case the buffer is sized for: longest host plus the
        // longest port the SwUrlParts field can carry (7 characters).
        char port[16];
        memset(port, '8', port_max);
        port[port_max] = 0;

        n = swHttpBuildGet(out, sizeof(out), host, port, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n > 0, "max host + max port: expected a built request, got %d", n);

        snprintf(want, sizeof(want), "Host: %s:%s\r\n", host, port);
        CHECK(n > 0 && strstr(out, want) != NULL,
              "max host + max port must reach Host untruncated");
    }
    {
        // One character past the host bound. Must be refused.
        char over[SW_URL_HOST_MAX + 8];
        memset(over, 'a', host_max + 1);
        over[host_max + 1] = 0;

        n = swHttpBuildGet(out, sizeof(out), over, NULL, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n == -1, "host one char over the bound: expected -1, got %d", n);
    }
    {
        // Max host plus a port past the port bound - the combined hostline is
        // what overflows, so this must be refused too.
        //
        // `host` here carries no ':', so it takes the non-bracketed "%s:%s"
        // path, whose capacity is host_max + ':' + port_len + NUL. The
        // hostline buffer now reserves two extra bytes for an IPv6 literal's
        // brackets (see swHttpBuildGet()), and those two bytes are unused
        // slack on this non-bracketed path - so port_max + 1 (the old
        // one-character-over probe) now fits instead of overflowing. The
        // probe is widened to port_max + 3 so it still lands one character
        // past the buffer's actual current capacity (266 bytes: 255 host +
        // '[' + ']' + ':' + 7 port + NUL) rather than the pre-widening one.
        char port[16];
        memset(port, '8', port_max + 3);
        port[port_max + 3] = 0;

        n = swHttpBuildGet(out, sizeof(out), host, port, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n == -1, "max host + over-long port: expected -1, got %d", n);
    }
}

static void test_build_get_ipv6_host(void)
{
    printf("swHttpBuildGet: an IPv6 literal is bracketed in Host, exactly once\n");

    char out[1024];
    int n;

    {
        // swUrlSplit() always hands this function the literal with its
        // brackets already stripped (SwUrlParts.host - see url.h: "no
        // brackets, even for an IPv6 literal"), so this is the exact shape
        // the builder actually receives from the app. RFC 7230 requires the
        // brackets back on in the Host header, or a server cannot tell where
        // the address ends and the port begins.
        n = swHttpBuildGet(out, sizeof(out), "2001:db8::1", "8000", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: [2001:db8::1]:8000\r\n") != NULL,
              "IPv6 + non-default port must be bracketed, got \"%s\"", out);
    }
    {
        // Default port must be omitted, same rule as any other host - but the
        // brackets still belong on their own, since "Host: 2001:db8::1" alone
        // is not itself illegal, but the moment a port is anywhere in the
        // picture it becomes ambiguous, so the rule is applied unconditionally
        // rather than only when a port is present.
        n = swHttpBuildGet(out, sizeof(out), "2001:db8::1", "443", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: [2001:db8::1]\r\n") != NULL,
              "IPv6 on default port must be bracketed with no port suffix, got \"%s\"", out);

        n = swHttpBuildGet(out, sizeof(out), "2001:db8::1", NULL, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: [2001:db8::1]\r\n") != NULL,
              "IPv6 with no port given must be bracketed with no port suffix, got \"%s\"", out);
    }
    {
        // A host that already carries brackets - not a shape swUrlSplit() ever
        // produces, but this function has no way to know who is calling it -
        // must not be bracketed a second time.
        n = swHttpBuildGet(out, sizeof(out), "[2001:db8::1]", "8000", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: [2001:db8::1]:8000\r\n") != NULL,
              "already-bracketed host must come through as-is, got \"%s\"", out);
        CHECK(n >= 0 && strstr(out, "[[") == NULL,
              "already-bracketed host must not gain a second '[', got \"%s\"", out);
    }
    {
        // Regression guard: a plain hostname and an IPv4 literal carry no ':'
        // and must come through exactly as before this fix - unbracketed.
        n = swHttpBuildGet(out, sizeof(out), "example.com", "8000", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: example.com:8000\r\n") != NULL,
              "plain hostname must stay unbracketed, got \"%s\"", out);

        n = swHttpBuildGet(out, sizeof(out), "192.0.2.1", "8000", "/x",
                           "UA", NULL, false, NULL);
        CHECK(n >= 0 && strstr(out, "Host: 192.0.2.1:8000\r\n") != NULL,
              "IPv4 literal must stay unbracketed, got \"%s\"", out);
    }
    {
        // The longest colon-bearing host the SW_URL_HOST_MAX bound allows
        // (255 chars) plus the longest port SwUrlParts::port can carry
        // (7 chars) is the exact worst case the widened hostline buffer is
        // sized for: 255 host + '[' + ']' + ':' + 7 port + NUL = 266 bytes.
        // This is the case that would overflow the pre-fix 264-byte buffer
        // once brackets are added, so it is the test that actually exercises
        // the widened bound rather than just the pre-existing host-length
        // check.
        const size_t host_max = SW_URL_HOST_MAX - 1;
        const size_t port_max = sizeof(((SwUrlParts *)0)->port) - 1;

        char host[SW_URL_HOST_MAX + 8];
        memset(host, ':', host_max);   // any colon-bearing content triggers
                                        // the IPv6 path; the builder does not
                                        // validate address syntax.
        host[host_max] = 0;

        char port[16];
        memset(port, '8', port_max);
        port[port_max] = 0;

        char out2[1024];
        n = swHttpBuildGet(out2, sizeof(out2), host, port, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n > 0, "max colon-bearing host + max port: expected a built request, got %d", n);

        char want[SW_URL_HOST_MAX + 32];
        snprintf(want, sizeof(want), "Host: [%s]:%s\r\n", host, port);
        CHECK(n > 0 && strstr(out2, want) != NULL,
              "max colon-bearing host + max port must reach Host untruncated and bracketed");

        // One character over the host bound must be refused, not truncated -
        // same rule as the non-IPv6 case in test_build_get_host_bound().
        char over[SW_URL_HOST_MAX + 8];
        memset(over, ':', host_max + 1);
        over[host_max + 1] = 0;
        n = swHttpBuildGet(out2, sizeof(out2), over, port, "/x",
                           "UA", NULL, false, NULL);
        CHECK(n == -1, "colon-bearing host one char over the bound: expected -1, got %d", n);
    }
}

// ------------------------------------------------------------- chunked feed

static void test_chunked_whole_body_one_call(void)
{
    printf("a whole chunked body decodes in a single feed\n");

    SwChunked c;
    swChunkedInit(&c);

    const unsigned char in[] = "5\r\nhello\r\n0\r\n\r\n";
    size_t in_len = sizeof(in) - 1;
    unsigned char out[64];
    size_t consumed = 0;

    int n = swChunkedFeed(&c, in, in_len, &consumed, out, sizeof(out));
    CHECK(n == 5, "expected 5 decoded bytes, got %d", n);
    CHECK(consumed == in_len, "expected all %zu bytes consumed, got %zu",
          in_len, consumed);
    CHECK(n == 5 && memcmp(out, "hello", 5) == 0, "decoded bytes wrong");
    CHECK(c.done, "the terminating 0-chunk must set done");
}

static void test_chunked_size_split_across_calls(void)
{
    printf("a chunk size split across two feeds still accumulates correctly\n");

    SwChunked c;
    swChunkedInit(&c);

    // Chunk size 0x1A = 26, split as "1" then "A\r\n" + 26 bytes + trailer.
    const unsigned char part1[] = "1";
    unsigned char out[64];
    size_t consumed = 0;

    int n = swChunkedFeed(&c, part1, sizeof(part1) - 1, &consumed, out, sizeof(out));
    CHECK(n == 0, "first half of the size: expected 0 bytes decoded, got %d", n);
    CHECK(consumed == 1, "first half of the size: expected 1 byte consumed, got %zu",
          consumed);

    const unsigned char part2[] = "A\r\nABCDEFGHIJKLMNOPQRSTUVWXYZ\r\n0\r\n\r\n";
    size_t part2_len = sizeof(part2) - 1;
    n = swChunkedFeed(&c, part2, part2_len, &consumed, out, sizeof(out));
    CHECK(n == 26, "expected 26 decoded bytes, got %d", n);
    CHECK(consumed == part2_len, "expected all %zu bytes consumed, got %zu",
          part2_len, consumed);
    CHECK(n == 26 && memcmp(out, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 26) == 0,
          "decoded bytes wrong");
    CHECK(c.done, "the terminating 0-chunk must set done");
}

static void test_chunked_data_split_across_calls(void)
{
    printf("a chunk's data split across two feeds still reassembles\n");

    SwChunked c;
    swChunkedInit(&c);

    const unsigned char part1[] = "5\r\nhel"; // size line + first 3 data bytes
    unsigned char out[64];
    size_t consumed = 0;

    int n = swChunkedFeed(&c, part1, sizeof(part1) - 1, &consumed, out, sizeof(out));
    CHECK(n == 3, "first half of data: expected 3 bytes, got %d", n);
    CHECK(n == 3 && memcmp(out, "hel", 3) == 0, "first half bytes wrong");
    CHECK(consumed == sizeof(part1) - 1, "first half: expected all consumed, got %zu",
          consumed);
    CHECK(!c.done, "must not be done mid-chunk");

    const unsigned char part2[] = "lo\r\n0\r\n\r\n";
    n = swChunkedFeed(&c, part2, sizeof(part2) - 1, &consumed, out, sizeof(out));
    CHECK(n == 2, "second half of data: expected 2 bytes, got %d", n);
    CHECK(n == 2 && memcmp(out, "lo", 2) == 0, "second half bytes wrong");
    CHECK(c.done, "the terminating 0-chunk must set done");
}

static void test_chunked_small_out_cap_is_resumable(void)
{
    printf("out_cap smaller than available data reports consumed and resumes\n");

    SwChunked c;
    swChunkedInit(&c);

    // One 10-byte chunk, fed all at once, but the caller's output buffer can
    // only take 4 bytes at a time.
    const unsigned char in[] = "A\r\n0123456789\r\n0\r\n\r\n";
    size_t in_len = sizeof(in) - 1;
    unsigned char out[4];
    size_t consumed = 0;

    int n = swChunkedFeed(&c, in, in_len, &consumed, out, sizeof(out));
    CHECK(n == 4, "first call: expected 4 bytes (out_cap), got %d", n);
    CHECK(n == 4 && memcmp(out, "0123", 4) == 0, "first call bytes wrong");
    CHECK(consumed > 0 && consumed < in_len,
          "first call must consume only part of the input, got %zu of %zu",
          consumed, in_len);
    CHECK(!c.done, "must not be done with data still pending");

    // Resume with whatever is left of the input and a fresh output buffer.
    size_t remaining = in_len - consumed;
    unsigned char out2[64];
    size_t consumed2 = 0;
    int n2 = swChunkedFeed(&c, in + consumed, remaining, &consumed2, out2, sizeof(out2));
    CHECK(n2 == 6, "second call: expected the remaining 6 bytes, got %d", n2);
    CHECK(n2 == 6 && memcmp(out2, "456789", 6) == 0, "second call bytes wrong");
    CHECK(consumed2 == remaining, "second call: expected all remaining input consumed");
    CHECK(c.done, "the terminating 0-chunk must set done");
}

static void test_chunked_malformed_size(void)
{
    printf("a malformed chunk size is refused, and stays refused\n");

    SwChunked c;
    swChunkedInit(&c);

    const unsigned char in[] = "G\r\n"; // 'G' is not hex and not a delimiter
    unsigned char out[16];
    size_t consumed = 0;

    int n = swChunkedFeed(&c, in, sizeof(in) - 1, &consumed, out, sizeof(out));
    CHECK(n == -1, "malformed size: expected -1, got %d", n);

    // Once dead, it must keep failing rather than silently recovering.
    const unsigned char more[] = "5\r\nhello\r\n0\r\n\r\n";
    n = swChunkedFeed(&c, more, sizeof(more) - 1, &consumed, out, sizeof(out));
    CHECK(n == -1, "feeding a dead decoder again: expected -1, got %d", n);
}

int main(void)
{
    printf("== httpmsg ==\n");

    test_status_lines();
    test_line_endings();
    test_incremental_feed();
    test_split_mid_header_line();
    test_body_off_with_body_present();
    test_rejects_bad_status_lines();
    test_rejects_wrong_digit_counts();
    test_rejects_out_of_range_codes();
    test_head_max_size();
    test_crlf_terminator_at_buffer_end();

    test_head_value();
    test_head_int();
    test_head_chunked();
    test_hls_manifest_detection();

    test_build_get();
    test_build_get_host_bound();
    test_build_get_ipv6_host();

    test_chunked_whole_body_one_call();
    test_chunked_size_split_across_calls();
    test_chunked_data_split_across_calls();
    test_chunked_small_out_cap_is_resumable();
    test_chunked_malformed_size();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
