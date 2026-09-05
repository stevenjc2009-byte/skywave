// Host tests for the SHOUTcast/Icecast metadata demultiplexer.
//
// Two levels of check:
//
//   1. A synthetic stream, built here, where every audio byte is a known value.
//      Stripping is verified byte-exact. This ships in the repo and needs no
//      network - and, importantly, no captured broadcast audio, which would be
//      somebody else's copyright.
//
//   2. Optionally, a real capture passed as argv[1]. That path walks the MPEG
//      frame chain through the demuxed output: if the demuxer miscounts by even
//      one byte the chain desynchronises and the walk fails. Run it with
//      a live capture to prove the thing works against a real server:
//
//        curl -H "Icy-MetaData: 1" --max-filesize 400000
//             http://media-ice.musicradio.com/ClassicFMMP3 -o /tmp/cap.bin
//        ./test_icy /tmp/cap.bin
//
// Build: see tests/Makefile.

#include "../source/net/icy.h"

#include <stdio.h>
#include <stdlib.h>
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

// ------------------------------------------------------------------ synthetic

// Builds a stream of `blocks` metadata blocks with `metaint` audio bytes
// between each. Audio byte i has value (i & 0xFF), so the expected demuxed
// output is a known sequence and any misalignment shows up immediately.
// `titles[b]` is the StreamTitle written into block b, or NULL for an empty
// block (length byte 0), which is what a real server sends most of the time.
static size_t build_stream(unsigned char *out, int metaint, int blocks,
                           const char *const *titles, size_t *audio_total)
{
    size_t o = 0, a = 0;

    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < metaint; i++) out[o++] = (unsigned char)(a++ & 0xFF);

        if (!titles[b]) {
            out[o++] = 0;  // empty block
        } else {
            char text[ICY_META_MAX + 1];
            snprintf(text, sizeof(text), "StreamTitle='%s';StreamUrl='';", titles[b]);
            size_t len   = strlen(text);
            int    units = (int)((len + 15) / 16);          // round up to 16 bytes
            size_t pad   = (size_t)units * 16;

            out[o++] = (unsigned char)units;
            memcpy(out + o, text, len);
            memset(out + o + len, 0, pad - len);            // servers zero-pad
            o += pad;
        }
    }

    // A trailing partial run of audio, so the demuxer is left mid-state at EOF
    // exactly as it would be when a read lands in the middle of a block.
    for (int i = 0; i < metaint / 3; i++) out[o++] = (unsigned char)(a++ & 0xFF);

    *audio_total = a;
    return o;
}

static void test_parse_title(void)
{
    printf("icyParseTitle\n");
    char t[ICY_TITLE_MAX];

    CHECK(icyParseTitle("StreamTitle='Hello';StreamUrl='';", t, sizeof(t)), "should parse");
    CHECK(strcmp(t, "Hello") == 0, "got '%s'", t);

    // An apostrophe inside a title is the case a naive "read to the next quote"
    // parser gets wrong, and track titles are full of them.
    CHECK(icyParseTitle("StreamTitle='Don't Stop Me Now';StreamUrl='';", t, sizeof(t)), "should parse");
    CHECK(strcmp(t, "Don't Stop Me Now") == 0, "apostrophe title truncated: '%s'", t);

    CHECK(icyParseTitle("StreamTitle='';", t, sizeof(t)), "empty title still parses");
    CHECK(strcmp(t, "") == 0, "got '%s'", t);

    CHECK(!icyParseTitle("StreamUrl='x';", t, sizeof(t)), "no StreamTitle key -> false");

    // Unterminated value must not read off the end of the buffer.
    CHECK(icyParseTitle("StreamTitle='no terminator", t, sizeof(t)), "unterminated still parses");
    CHECK(strcmp(t, "no terminator") == 0, "got '%s'", t);
}

static void test_passthrough_when_no_metadata(void)
{
    printf("metaint == 0 is a pure passthrough\n");
    IcyDemux d;
    icyInit(&d, 0);

    unsigned char in[500], out[500];
    for (int i = 0; i < 500; i++) in[i] = (unsigned char)(i & 0xFF);

    size_t n = icyFeed(&d, in, sizeof(in), out);
    CHECK(n == sizeof(in), "expected %zu bytes, got %zu", sizeof(in), n);
    CHECK(memcmp(in, out, sizeof(in)) == 0, "passthrough altered the bytes");
}

// Feeds the synthetic stream in chunks of `chunk` bytes. Varying the chunk size
// is the point: a real socket hands over arbitrary boundaries, and a demuxer
// that only works when a block arrives whole is broken in the field.
static void test_synthetic(int metaint, size_t chunk)
{
    static const char *titles[] = {
        NULL,                       // empty block, the common case
        "Georges Bizet - Carmen",
        NULL,
        "Georges Bizet - Carmen",   // repeat: must NOT re-fire title_changed
        "Gustav Holst - Jupiter",
        NULL,
    };
    const int blocks = (int)(sizeof(titles) / sizeof(titles[0]));

    static unsigned char stream[1 << 20];
    size_t audio_total = 0;
    size_t stream_len  = build_stream(stream, metaint, blocks, titles, &audio_total);

    IcyDemux d;
    icyInit(&d, metaint);

    static unsigned char out[1 << 20];
    size_t produced = 0;
    int    titles_seen = 0;
    char   last_title[ICY_TITLE_MAX] = {0};

    for (size_t off = 0; off < stream_len; off += chunk) {
        size_t n = stream_len - off < chunk ? stream_len - off : chunk;
        produced += icyFeed(&d, stream + off, n, out + produced);

        char t[ICY_TITLE_MAX];
        if (icyTakeTitle(&d, t, sizeof(t))) {
            titles_seen++;
            snprintf(last_title, sizeof(last_title), "%s", t);
        }
    }

    CHECK(produced == audio_total,
          "metaint=%d chunk=%zu: expected %zu audio bytes, got %zu",
          metaint, chunk, audio_total, produced);

    // Byte-exact: every metadata byte removed, every audio byte kept, in order.
    int mismatch = -1;
    for (size_t i = 0; i < produced; i++) {
        if (out[i] != (unsigned char)(i & 0xFF)) { mismatch = (int)i; break; }
    }
    CHECK(mismatch < 0, "metaint=%d chunk=%zu: audio corrupted at byte %d",
          metaint, chunk, mismatch);

    // Two DISTINCT titles appear across six blocks, and one of them is sent
    // twice in a row. A repeat must never be reported as a change, or the UI
    // redraws on every metadata block - which is every few seconds.
    //
    // The count is an upper bound rather than an equality because the caller
    // polls once per chunk: when a chunk is big enough to swallow two changes,
    // the second overwrites the first before it is collected. That is correct
    // for a now-playing display (latest wins), so what matters is that it never
    // exceeds the number of real changes, and that the final value is right.
    CHECK(titles_seen >= 1 && titles_seen <= 2,
          "metaint=%d chunk=%zu: expected 1-2 title changes, got %d "
          "(3 would mean the repeated title re-fired)",
          metaint, chunk, titles_seen);
    CHECK(strcmp(last_title, "Gustav Holst - Jupiter") == 0,
          "metaint=%d chunk=%zu: last title was '%s'", metaint, chunk, last_title);
}

// Polls after every single block, so no change can be missed by coarse
// sampling. This is the test that actually pins repeat-suppression: feed the
// same title twice and it must fire exactly once.
static void test_repeat_suppressed(void)
{
    printf("a repeated title does not re-fire\n");

    const int metaint = 1024;
    static const char *titles[] = {
        "Track One",   // change 1
        "Track One",   // repeat - must be silent
        "Track One",   // repeat - must be silent
        "Track Two",   // change 2
        "Track One",   // change 3: back to an earlier title is still a change
    };
    const int blocks = (int)(sizeof(titles) / sizeof(titles[0]));

    static unsigned char stream[1 << 16];
    static unsigned char out[1 << 16];
    size_t audio_total = 0;
    build_stream(stream, metaint, blocks, titles, &audio_total);

    IcyDemux d;
    icyInit(&d, metaint);

    // Feed one byte at a time and poll after each, so every change is visible.
    size_t produced = 0, fired = 0;
    char last[ICY_TITLE_MAX] = {0}, t[ICY_TITLE_MAX];
    size_t stream_len = 0;
    {
        // Recompute the exact encoded length the builder produced.
        size_t audio_dummy = 0;
        stream_len = build_stream(stream, metaint, blocks, titles, &audio_dummy);
    }

    for (size_t i = 0; i < stream_len; i++) {
        produced += icyFeed(&d, stream + i, 1, out + produced);
        if (icyTakeTitle(&d, t, sizeof(t))) {
            fired++;
            snprintf(last, sizeof(last), "%s", t);
        }
    }

    CHECK(fired == 3, "expected exactly 3 title changes from 5 blocks, got %zu", fired);
    CHECK(strcmp(last, "Track One") == 0, "final title was '%s'", last);
}

// -------------------------------------------------------------- real capture

// Minimal MPEG-1/2 Layer III frame length calculator - enough to walk a chain.
static const int kBitrateV1L3[16] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
};
static const int kBitrateV2L3[16] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
};
static const int kSampleRate[4][3] = {
    { 11025, 12000,  8000 },  // MPEG 2.5
    {     0,     0,     0 },  // reserved
    { 22050, 24000, 16000 },  // MPEG 2
    { 44100, 48000, 32000 },  // MPEG 1
};

// Returns the frame length in bytes, or 0 if `h` is not a valid Layer III header.
static int frame_len(const unsigned char *h)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return 0;

    int ver_id  = (h[1] >> 3) & 0x03;
    int layer   = (h[1] >> 1) & 0x03;
    int bri     = (h[2] >> 4) & 0x0F;
    int sri     = (h[2] >> 2) & 0x03;
    int pad     = (h[2] >> 1) & 0x01;

    if (ver_id == 1 || layer != 1) return 0;      // reserved version / not Layer III
    if (bri == 0 || bri == 15 || sri == 3) return 0;

    int rate = (ver_id == 3 ? kBitrateV1L3[bri] : kBitrateV2L3[bri]) * 1000;
    int freq = kSampleRate[ver_id][sri];
    if (!rate || !freq) return 0;

    int spf = (ver_id == 3) ? 1152 : 576;         // samples per frame
    return (spf / 8) * rate / freq + pad;
}

// Walks the frame chain from the first sync word. Returns the number of frames
// that chained cleanly, and sets *reached to how far into the buffer it got.
static int walk_frames(const unsigned char *d, size_t len, size_t *reached)
{
    size_t i = 0;
    while (i + 4 < len && frame_len(d + i) == 0) i++;   // find first sync

    int frames = 0;
    while (i + 4 < len) {
        int fl = frame_len(d + i);
        if (fl <= 0) break;
        i += (size_t)fl;
        frames++;
    }
    *reached = i;
    return frames;
}

static void test_real_capture(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("  (no capture at %s - skipping)\n", path); return; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *raw = malloc((size_t)sz);
    unsigned char *aud = malloc((size_t)sz);
    if (!raw || !aud || fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        printf("  FAIL could not read %s\n", path); failures++; checks++;
        free(raw); free(aud); fclose(f); return;
    }
    fclose(f);

    // Classic FM's measured icy-metaint. Fed in awkward 1000-byte chunks so
    // metadata blocks straddle chunk boundaries.
    IcyDemux d;
    icyInit(&d, 8000);

    size_t produced = 0;
    int    titles_seen = 0;
    char   first_title[ICY_TITLE_MAX] = {0};

    for (long off = 0; off < sz; off += 1000) {
        size_t n = (size_t)(sz - off < 1000 ? sz - off : 1000);
        produced += icyFeed(&d, raw + off, n, aud + produced);

        char t[ICY_TITLE_MAX];
        if (icyTakeTitle(&d, t, sizeof(t))) {
            if (!titles_seen) snprintf(first_title, sizeof(first_title), "%s", t);
            titles_seen++;
        }
    }

    printf("  capture: %ld bytes in -> %zu audio bytes out (%ld stripped)\n",
           sz, produced, sz - (long)produced);
    if (titles_seen) printf("  title seen: \"%s\"\n", first_title);

    CHECK(produced < (size_t)sz, "nothing was stripped - metadata was not detected");
    CHECK(titles_seen >= 1, "no StreamTitle recovered from a real stream");

    // The real proof. A correctly demuxed Icecast body is one unbroken MPEG
    // frame chain; a demuxer that miscounts leaves metadata bytes inline and
    // the walk stops early.
    size_t reached_ok = 0;
    int frames_ok = walk_frames(aud, produced, &reached_ok);
    // The final frame usually runs past the end of a truncated capture, which
    // would otherwise report as slightly over 100%.
    if (reached_ok > produced) reached_ok = produced;
    double covered_ok = 100.0 * (double)reached_ok / (double)produced;
    printf("  demuxed : %d frames, chain reached %.2f%% of the buffer\n",
           frames_ok, covered_ok);
    CHECK(covered_ok > 99.0,
          "demuxed frame chain broke at %.2f%% - stripping is misaligned", covered_ok);

    // Negative control. The same walk over the RAW bytes (metadata left in)
    // must break early. If this also passes, the check above proves nothing.
    size_t reached_raw = 0;
    int frames_raw = walk_frames(raw, (size_t)sz, &reached_raw);
    double covered_raw = 100.0 * (double)reached_raw / (double)sz;
    printf("  control : %d frames, chain reached %.2f%% of the raw buffer\n",
           frames_raw, covered_raw);
    CHECK(covered_raw < 99.0,
          "CONTROL DID NOT GO RED: raw stream also walked cleanly (%.2f%%), "
          "so the frame-chain check cannot detect a broken demuxer", covered_raw);

    free(raw);
    free(aud);
}

int main(int argc, char **argv)
{
    printf("== icy ==\n");
    test_parse_title();
    test_passthrough_when_no_metadata();
    test_repeat_suppressed();

    printf("synthetic streams across chunk boundaries\n");
    // Chunk sizes chosen to straddle every boundary: smaller than a block,
    // larger than metaint, coprime with both, and exactly metaint.
    size_t chunks[] = { 1, 7, 13, 1000, 4096, 8000, 8191, 65536 };
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        test_synthetic(8000, chunks[i]);
        test_synthetic(16000, chunks[i]);
    }

    printf("real capture\n");
    test_real_capture(argc > 1 ? argv[1] : "capture.bin");

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
