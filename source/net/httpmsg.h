#pragma once

// HTTP message framing: build a request, parse a response head, decode a
// chunked body.
//
// Kept free of any 3DS header, like url.c, so all of it can be proven on a PC
// in a second. That is the whole reason this is a separate file rather than
// living inside http.c: Skywave stopped using libctru's httpc, which means the
// app now owns the parts of HTTP that httpc used to own, and a parser bug would
// otherwise only be findable on a console with no log.
//
// Written to be tolerant in the two places real servers are sloppy, and strict
// everywhere else:
//
//   * The status line may say `ICY 200 OK` instead of `HTTP/1.x 200 OK`.
//     Shoutcast v1 servers still answer that way, and there are plenty in the
//     directory. A parser that rejects it turns a working station into a dead
//     one.
//   * The header block may end with bare LF rather than CRLF.

#include <stdbool.h>
#include <stddef.h>

// Big enough for a real response head with a long cookie or CSP block. A head
// larger than this is refused rather than truncated: a truncated head can hide
// the Location or the Transfer-Encoding, which is a silent wrong answer.
#define SW_HEAD_MAX 4096

typedef struct {
    int    status;              // the code from the status line
    bool   icy;                 // the status line said ICY, not HTTP
    char   raw[SW_HEAD_MAX];    // header lines only, NUL-terminated
    size_t raw_len;
} SwHttpHead;

typedef enum {
    SW_HEAD_BAD       = -1,     // not an HTTP response, or the head is too big
    SW_HEAD_NEED_MORE = 0,      // no blank line yet; read more and call again
    SW_HEAD_OK        = 1,
} SwHeadState;

// Parses a response head out of the front of `buf`.
//
// Call it again with more data each time it returns SW_HEAD_NEED_MORE. On
// SW_HEAD_OK, `*body_off` is the offset in `buf` of the first body byte, so the
// caller can keep whatever it over-read.
SwHeadState swHttpHeadParse(const char *buf, size_t len,
                            SwHttpHead *out, size_t *body_off);

// Copies the value of a header into `out`, NUL-terminated, with surrounding
// whitespace trimmed. Name matching is case-insensitive (RFC 7230 says field
// names are, and `icy-metaint` really does arrive as `Icy-MetaInt`). Returns
// false if the header is absent or would not fit.
bool swHttpHeadValue(const SwHttpHead *h, const char *name,
                     char *out, size_t cap);

// The value as an integer, or `fallback` if absent or not a number.
long swHttpHeadInt(const SwHttpHead *h, const char *name, long fallback);

// True if the response says its body is chunked.
bool swHttpHeadChunked(const SwHttpHead *h);

// True if this response is an HLS/M3U8 manifest - a text playlist of segment
// URLs - rather than a playable audio stream.
//
// Checked two ways because neither alone is trustworthy: a directory entry can
// report a playable codec (AAC+) for a station that is HLS underneath (MEASURED
// 2026-09-07 against two live BBC Radio 1 entries - see http.c's call site for
// the numbers), so Content-Type is consulted first but not relied on alone -
// some servers omit it or get it wrong, the same reason player.c sniffs AAC by
// its ADTS sync rather than trusting a header. `body` is whatever of the
// response has been read so far and may be empty; that alone is not evidence of
// HLS, only a real "#EXTM3U" magic is.
bool swHttpIsHlsManifest(const SwHttpHead *h, const char *body, size_t body_len);

// Writes a complete request into `out`. Returns the byte count, or -1 if it
// would not fit. `extra` may be NULL; when given it must already be CRLF-
// terminated lines.
int swHttpBuildGet(char *out, size_t cap,
                   const char *host, const char *port, const char *path,
                   const char *user_agent, const char *authorization,
                   bool want_icy_meta, const char *extra);

// ---- chunked transfer decoding -------------------------------------------
//
// Incremental because the caller is streaming: bytes arrive in whatever sizes
// the network hands over, and a chunk header can be split across two reads.

typedef struct {
    int           state;      // internal
    unsigned long remain;     // bytes left in the current chunk
    bool          done;       // the terminating zero-length chunk was seen
} SwChunked;

void swChunkedInit(SwChunked *c);

// Decodes as much of `in` as fits in `out`.
//
// Returns the number of decoded body bytes written to `out`, or -1 if the
// stream is malformed. `*consumed` is set to how many input bytes were eaten,
// which may be less than `in_len` when `out` filled up - call again with the
// rest. `c->done` becomes true at the end of the body.
int swChunkedFeed(SwChunked *c, const unsigned char *in, size_t in_len,
                  size_t *consumed, unsigned char *out, size_t out_cap);
