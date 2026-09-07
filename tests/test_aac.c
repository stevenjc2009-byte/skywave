// Host tests for the Helix AAC bridge (source/audio/aac_bridge.c), the thin
// wrapper around the vendored RealNetworks Helix decoder (source/audio/aac/,
// RPSL-licensed - see source/audio/aac/README.md) that lets player.c decode
// AAC/ADTS streams the same way it already decodes MP3 through mpg123.
//
// This decodes a REAL, deterministic AAC-LC ADTS stream (test_aac_fixture.h -
// a 1 kHz sine tone, 44100 Hz mono, 0.5 s, produced offline by ffmpeg's own
// native AAC encoder; see that file's header comment for the exact command)
// and checks the actual decoded PCM, not just a status code: exact frame
// count (via the number of AACDEC_NEW_FORMAT/OK bytes decoded), sample rate,
// channel count, and an FNV-1a checksum of the decoded samples. The expected
// numbers below were captured by running this exact code against this exact
// fixture once (see the code-vault log for the run) - if a future change to
// aac_bridge.c or the vendored decoder legitimately changes the decoded
// output, these constants need to be re-measured and re-committed, not just
// bumped to make the test pass.

#include "../source/audio/aac_bridge.h"
#include "test_aac_fixture.h"

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
            printf(__VA_ARGS__);                             \
            printf("\n");                                   \
        }                                                   \
    } while (0)

// Measured against the real fixture - see the file header comment.
#define EXPECT_FRAMES      23
#define EXPECT_RATE        44100
#define EXPECT_CHANNELS    1
#define EXPECT_TOTAL_BYTES 47104u   // 23 frames * 1024 samples * 2 bytes
#define EXPECT_CHECKSUM    0x28A64425u
#define MIN_SUM_ABS         10000000ll  // real signal energy, not near-silence

// Same chunk size player.c's fill() pulls off the ring per call
// (FEED_CHUNK in source/audio/player.c), so this exercises the bridge the
// same way it is actually driven, not with the whole file in one call.
#define FEED_CHUNK 4096

static uint32_t fnv1a_pcm(const int16_t *pcm, size_t nsamples)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < nsamples; i++) {
        uint16_t s = (uint16_t)pcm[i];
        h ^= (uint8_t)(s & 0xFF);       h *= 16777619u;
        h ^= (uint8_t)((s >> 8) & 0xFF); h *= 16777619u;
    }
    return h;
}

// Feeds `data`/`len` through `b` in FEED_CHUNK pieces (mirroring player.c's
// fill_aac loop, minus the DSP-wavebuf-boundary bookkeeping that only matters
// mid-stream on a real, continuously-buffering player), draining any
// trailing pending/staged frame afterwards. Returns the decoded PCM byte
// count and, via the out-params, how many NEW_FORMAT announcements fired and
// the last rate/channels reported.
static size_t decode_all(AacBridge *b, const uint8_t *data, size_t len,
                          int16_t *out, size_t out_cap,
                          int *new_format_count, long *last_rate, int *last_channels)
{
    size_t done_total = 0;
    size_t offset = 0;

    *new_format_count = 0;
    *last_rate = 0;
    *last_channels = 0;

    while (offset < len) {
        size_t chunk = len - offset;
        if (chunk > FEED_CHUNK) chunk = FEED_CHUNK;

        size_t done = 0;
        long rate; int channels;
        AacDecStatus st = aacBridgeDecode(b, data + offset, chunk,
                                          (int16_t *)((uint8_t *)out + done_total),
                                          out_cap - done_total, &done,
                                          &rate, &channels);
        offset += chunk;
        done_total += done;

        if (st == AACDEC_NEW_FORMAT) {
            (*new_format_count)++;
            *last_rate = rate;
            *last_channels = channels;
        } else if (st == AACDEC_ERROR) {
            printf("  (decode_all: AACDEC_ERROR at offset %zu)\n", offset);
            break;
        }
    }

    // Drain: a format-changing frame found near the end of the input can
    // still be sitting in `b` waiting to be delivered (see aac_bridge.h on
    // aacBridgeHasPending) - keep calling with no new input until nothing
    // more comes out, same as player.c would across successive fill() calls.
    for (;;) {
        size_t done = 0;
        long rate; int channels;
        AacDecStatus st = aacBridgeDecode(b, NULL, 0,
                                          (int16_t *)((uint8_t *)out + done_total),
                                          out_cap - done_total, &done,
                                          &rate, &channels);
        if (st == AACDEC_NEW_FORMAT) {
            (*new_format_count)++;
            *last_rate = rate;
            *last_channels = channels;
        }
        done_total += done;
        if (done == 0) break;
    }

    return done_total;
}

static void test_sniffing(void)
{
    printf("ADTS sniffing recognises real syncwords and rejects MP3/garbage\n");

    CHECK(aacBridgeLooksLikeAdts(TEST_AAC_FIXTURE, TEST_AAC_FIXTURE_LEN),
          "the real fixture's own first bytes must sniff as ADTS");

    // MPEG-1 Layer III (MP3) sync: 11-bit sync then layer bits 01. Must never
    // be mistaken for ADTS - see aac_bridge.h on why the two are mutually
    // exclusive by construction.
    uint8_t mp3_sync[2] = { 0xFF, 0xFB };
    CHECK(!aacBridgeLooksLikeAdts(mp3_sync, sizeof(mp3_sync)),
          "an MP3 frame sync must not sniff as ADTS");

    uint8_t mp3_sync2[2] = { 0xFF, 0xFA };
    CHECK(!aacBridgeLooksLikeAdts(mp3_sync2, sizeof(mp3_sync2)),
          "an MP3 (CRC-protected) frame sync must not sniff as ADTS");

    uint8_t garbage[2] = { 0x49, 0x44 }; // "ID" - e.g. an ID3 tag, not a sync at all
    CHECK(!aacBridgeLooksLikeAdts(garbage, sizeof(garbage)),
          "non-sync bytes must not sniff as ADTS");

    uint8_t too_short[1] = { 0xFF };
    CHECK(!aacBridgeLooksLikeAdts(too_short, sizeof(too_short)),
          "fewer than 2 bytes must not crash or claim a match");
}

static void test_decodes_real_stream(void)
{
    printf("decodes a real ADTS stream into the PCM it actually contains\n");

    AacBridge *b = aacBridgeNew();
    CHECK(b != NULL, "aacBridgeNew must succeed");
    if (!b) return;

    static int16_t out[1 << 16];
    int new_format_count; long rate; int channels;
    size_t total = decode_all(b, TEST_AAC_FIXTURE, TEST_AAC_FIXTURE_LEN,
                              out, sizeof(out), &new_format_count, &rate, &channels);

    CHECK(new_format_count == 1,
          "expected exactly one format announcement for a constant-format "
          "stream, got %d", new_format_count);
    CHECK(rate == EXPECT_RATE, "expected %d Hz, got %ld", EXPECT_RATE, rate);
    CHECK(channels == EXPECT_CHANNELS,
          "expected %d channel(s), got %d", EXPECT_CHANNELS, channels);
    CHECK(total == EXPECT_TOTAL_BYTES,
          "expected exactly %u decoded bytes (%d frames * 1024 samples * "
          "2 bytes), got %zu", EXPECT_TOTAL_BYTES, EXPECT_FRAMES, total);

    size_t nsamples = total / sizeof(int16_t);
    uint32_t checksum = fnv1a_pcm(out, nsamples);
    CHECK(checksum == EXPECT_CHECKSUM,
          "decoded PCM checksum changed: expected 0x%08X, got 0x%08X - "
          "either the decode path regressed, or this constant needs "
          "re-measuring against a deliberate change",
          EXPECT_CHECKSUM, checksum);

    long long sum_abs = 0;
    for (size_t i = 0; i < nsamples; i++) {
        int v = out[i];
        sum_abs += (v < 0) ? -v : v;
    }
    CHECK(sum_abs > MIN_SUM_ABS,
          "decoded PCM looks like near-silence (sum|x|=%lld) - a sniffing "
          "false-positive would decode to garbage/silence like this, not "
          "return an error", sum_abs);

    aacBridgeFree(b);
}

// The staging buffer has to resync from arbitrary noise, since a station can
// glitch mid-stream. This prepends non-ADTS junk before the real fixture and
// checks the decoder still finds and decodes every real frame afterwards -
// exercising aac_bridge.c's byte-at-a-time resync path directly, not just the
// happy path where the very first bytes are already a valid syncword.
static void test_resyncs_after_garbage(void)
{
    printf("resyncs after leading garbage instead of giving up\n");

    static uint8_t junk_then_real[600 + 4539 /* fixture size, see below */];
    size_t junk_len = 600;
    for (size_t i = 0; i < junk_len; i++)
        junk_then_real[i] = (uint8_t)(0x11 * (i + 1)); // never happens to form 0xFF Fx

    CHECK(sizeof(junk_then_real) - junk_len == TEST_AAC_FIXTURE_LEN,
          "test buffer sizing drifted from the fixture size (%zu) - update "
          "the 4539 literal above to match", TEST_AAC_FIXTURE_LEN);
    memcpy(junk_then_real + junk_len, TEST_AAC_FIXTURE, TEST_AAC_FIXTURE_LEN);

    AacBridge *b = aacBridgeNew();
    CHECK(b != NULL, "aacBridgeNew must succeed");
    if (!b) return;

    static int16_t out[1 << 16];
    int new_format_count; long rate; int channels;
    size_t total = decode_all(b, junk_then_real, sizeof(junk_then_real),
                              out, sizeof(out), &new_format_count, &rate, &channels);

    CHECK(total == EXPECT_TOTAL_BYTES,
          "leading garbage should be skipped over, not corrupt or truncate "
          "the real stream after it - expected %u bytes, got %zu",
          EXPECT_TOTAL_BYTES, total);
    CHECK(rate == EXPECT_RATE && channels == EXPECT_CHANNELS,
          "format after resync should still read as %d Hz / %d ch, got "
          "%ld Hz / %d ch", EXPECT_RATE, EXPECT_CHANNELS, rate, channels);

    aacBridgeFree(b);
}

// swPlayerPlay() calls aacBridgeReset() on every station change (see
// player.c) instead of allocating a fresh bridge, so a stale tail from the
// previous station must not survive it. This proves reset actually clears
// staged bytes and decoder state by decoding the real fixture, resetting
// straight after (mid-nothing, as if a station change interrupted it), then
// decoding the exact same fixture again from scratch and checking it comes
// out identically - if reset left anything behind, the second decode would
// desync, drop the leading frame(s), or produce a different checksum.
static void test_reset_clears_state(void)
{
    printf("aacBridgeReset() leaves no trace of the previous station\n");

    AacBridge *b = aacBridgeNew();
    CHECK(b != NULL, "aacBridgeNew must succeed");
    if (!b) return;

    static int16_t out1[1 << 16];
    int nf1; long rate1; int ch1;
    size_t total1 = decode_all(b, TEST_AAC_FIXTURE, TEST_AAC_FIXTURE_LEN,
                               out1, sizeof(out1), &nf1, &rate1, &ch1);
    uint32_t checksum1 = fnv1a_pcm(out1, total1 / sizeof(int16_t));

    aacBridgeReset(b);

    static int16_t out2[1 << 16];
    int nf2; long rate2; int ch2;
    size_t total2 = decode_all(b, TEST_AAC_FIXTURE, TEST_AAC_FIXTURE_LEN,
                               out2, sizeof(out2), &nf2, &rate2, &ch2);
    uint32_t checksum2 = fnv1a_pcm(out2, total2 / sizeof(int16_t));

    CHECK(total1 == total2,
          "byte count changed after reset: %zu then %zu", total1, total2);
    CHECK(checksum1 == checksum2,
          "decoded PCM changed after reset (0x%08X then 0x%08X) - reset is "
          "leaking state from the previous station into the next one",
          checksum1, checksum2);
    CHECK(rate1 == rate2 && ch1 == ch2,
          "format changed after reset: %ld Hz/%d ch then %ld Hz/%d ch",
          rate1, ch1, rate2, ch2);

    aacBridgeFree(b);
}

// A partial frame must be waited for, never handed to the decoder.
//
// This is a regression guard for a console-freezing bug, so it is worth being
// explicit about what it guards. AACDecode does not bounds-check the frame body
// against the buffer length it is given: UnpackADTSHeader only fails if the
// 7-byte HEADER ran out, and DecodeNoiselessData then reads section data
// straight off the end. noiseless.c:137 is
//     do { sectLenIncr = GetBits(bsi, sectLenBits);
//          sectLen += sectLenIncr; } while (sectLenIncr == sectEscapeVal);
// whose only exit is a value that is not the escape value - so a bit reader
// running past the end and returning all-ones never leaves. Not a crash, not an
// error code: a spinning audio thread, i.e. a frozen console.
//
// The synthetic header below is the deterministic half: 21 bytes staged against
// a stated frame length of 254, which must come back as NEED_MORE with nothing
// decoded. It never reaches the decoder, so it cannot hang even when the guard
// is removed - it fails loudly instead.
static void test_short_frame_waits_for_the_rest(void)
{
    printf("a frame shorter than its stated length is waited for, not decoded\n");

    // 0xFF 0xF1 = syncword, MPEG-4, layer 00, no CRC. Then AAC-LC, 44100 Hz
    // (index 4), 1 channel, and a 13-bit frame_length of 254 split across
    // bytes 3-5 exactly as the fixed bridge reads it back.
    static const uint8_t header_only[21] = {
        0xFF, 0xF1, 0x50, 0x40, 0x1F, 0xDF, 0xFC,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    CHECK(aacBridgeLooksLikeAdts(header_only, sizeof(header_only)),
          "the synthetic header must sniff as ADTS, or this test is checking "
          "nothing");

    AacBridge *b = aacBridgeNew();
    CHECK(b != NULL, "aacBridgeNew must succeed");
    if (!b) return;

    static int16_t out[1 << 14];
    size_t done = 99;                 // poisoned: must be overwritten with 0
    long rate = -1; int channels = -1;

    AacDecStatus st = aacBridgeDecode(b, header_only, sizeof(header_only),
                                      out, sizeof(out), &done, &rate, &channels);

    CHECK(st == AACDEC_NEED_MORE,
          "21 bytes of a 254-byte frame must be NEED_MORE, got %d", (int)st);
    CHECK(done == 0, "nothing may be decoded from a partial frame, got %zu bytes",
          done);

    aacBridgeFree(b);
}

// The same guard, driven the way the network actually drives it.
//
// Truncation is not an edge case here - staging stops mid-frame on nearly every
// read, and whether that hangs depends only on what the bit reader finds past
// the end. Feeding the fixture in small, deliberately awkward chunk sizes walks
// a partial-frame boundary through many different offsets within the stream,
// and the decoded output must be bit-identical to the 4096-byte-chunk decode:
// waiting for the rest of a frame is not allowed to lose or alter a sample.
//
// If the guard is removed this test does not print a failure - it HANGS, and
// the suite never reaches its summary line. That is a real red signal (a suite
// that does not finish has not passed) but an unusual-looking one, so: a run
// that stops with no "N checks, N failures" line is this bug, not a flake.
// There is no portable alarm() to convert it into a clean failure - these tests
// also build under MinGW - so the shape of the failure is documented instead.
static void test_truncated_feeds_do_not_hang(void)
{
    printf("odd feed sizes hit partial-frame boundaries without hanging\n");

    static int16_t ref[1 << 16];
    int nf; long rate; int ch;
    AacBridge *b = aacBridgeNew();
    CHECK(b != NULL, "aacBridgeNew must succeed");
    if (!b) return;
    size_t ref_total = decode_all(b, TEST_AAC_FIXTURE, TEST_AAC_FIXTURE_LEN,
                                  ref, sizeof(ref), &nf, &rate, &ch);
    uint32_t ref_sum = fnv1a_pcm(ref, ref_total / sizeof(int16_t));
    aacBridgeFree(b);

    CHECK(ref_total == EXPECT_TOTAL_BYTES,
          "reference decode must match the measured fixture size (%u), got %zu "
          "- the comparison below is worthless otherwise",
          EXPECT_TOTAL_BYTES, ref_total);

    // Not round numbers, and none a divisor of the fixture's frame size: the
    // point is to land the buffer boundary at many different places inside a
    // frame rather than the same place every time.
    static const size_t kChunks[] = { 1, 7, 17, 23, 29, 31, 97, 251 };

    for (size_t c = 0; c < sizeof(kChunks) / sizeof(kChunks[0]); c++) {
        size_t chunk = kChunks[c];

        AacBridge *bb = aacBridgeNew();
        if (!bb) { CHECK(0, "aacBridgeNew must succeed"); return; }

        static int16_t got[1 << 16];
        size_t total = 0, offset = 0;

        while (offset < TEST_AAC_FIXTURE_LEN) {
            size_t n = TEST_AAC_FIXTURE_LEN - offset;
            if (n > chunk) n = chunk;

            size_t done = 0; long r; int cc;
            aacBridgeDecode(bb, TEST_AAC_FIXTURE + offset, n,
                            (int16_t *)((uint8_t *)got + total),
                            sizeof(got) - total, &done, &r, &cc);
            offset += n;
            total  += done;
        }
        for (;;) {                       // drain, as decode_all does
            size_t done = 0; long r; int cc;
            aacBridgeDecode(bb, NULL, 0, (int16_t *)((uint8_t *)got + total),
                            sizeof(got) - total, &done, &r, &cc);
            total += done;
            if (done == 0) break;
        }

        CHECK(total == ref_total,
              "feeding %zu bytes at a time decoded %zu bytes, not %zu",
              chunk, total, ref_total);
        CHECK(fnv1a_pcm(got, total / sizeof(int16_t)) == ref_sum,
              "feeding %zu bytes at a time changed the decoded PCM", chunk);

        aacBridgeFree(bb);
    }
}

int main(void)
{
    printf("== aac ==\n");
    test_sniffing();
    test_decodes_real_stream();
    test_resyncs_after_garbage();
    test_reset_clears_state();
    test_short_frame_waits_for_the_rest();
    test_truncated_feeds_do_not_hang();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
