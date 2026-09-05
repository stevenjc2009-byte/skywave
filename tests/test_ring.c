// Host tests for the single-producer/single-consumer ring buffer.
//
// The interesting cases are all at the boundaries: writing across the wrap,
// reading across the wrap, and the full/empty distinction that the one
// reserved byte exists to make possible.

#include "../source/audio/ring.h"

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

static void test_rejects_bad_capacity(void)
{
    printf("capacity must be a power of two\n");
    Ring r;
    static uint8_t buf[64];

    CHECK(ringInit(&r, buf, 64), "64 is a power of two");
    CHECK(!ringInit(&r, buf, 63), "63 must be rejected");
    CHECK(!ringInit(&r, buf, 48), "48 must be rejected");
    CHECK(!ringInit(&r, NULL, 64), "null buffer must be rejected");
}

static void test_empty_and_full(void)
{
    printf("empty and full are distinguishable\n");
    Ring r;
    static uint8_t buf[16];
    ringInit(&r, buf, 16);

    CHECK(ringUsed(&r) == 0, "fresh ring should be empty, used=%zu", ringUsed(&r));
    CHECK(ringFree(&r) == 15, "capacity is cap-1, free=%zu", ringFree(&r));

    uint8_t src[15];
    memset(src, 0xAB, sizeof(src));
    CHECK(ringWrite(&r, src, 15) == 15, "should take all 15");
    CHECK(ringUsed(&r) == 15, "used=%zu", ringUsed(&r));
    CHECK(ringFree(&r) == 0, "free=%zu", ringFree(&r));

    // Full: a further write must take nothing rather than silently wrapping
    // over unread data.
    CHECK(ringWrite(&r, src, 1) == 0, "write to a full ring must take 0");

    uint8_t dst[32];
    CHECK(ringRead(&r, dst, 32) == 15, "should return the 15 buffered bytes");
    CHECK(ringUsed(&r) == 0, "used=%zu after draining", ringUsed(&r));
    CHECK(ringRead(&r, dst, 1) == 0, "read from an empty ring must return 0");
}

static void test_short_counts(void)
{
    printf("short reads and writes report honestly\n");
    Ring r;
    static uint8_t buf[16];
    ringInit(&r, buf, 16);

    uint8_t src[32];
    for (int i = 0; i < 32; i++) src[i] = (uint8_t)i;

    // Asking to write 32 into a 15-byte capacity must take exactly 15.
    CHECK(ringWrite(&r, src, 32) == 15, "expected a short write of 15");

    uint8_t dst[32] = {0};
    CHECK(ringRead(&r, dst, 4) == 4, "partial read");
    for (int i = 0; i < 4; i++) CHECK(dst[i] == (uint8_t)i, "dst[%d]=%d", i, dst[i]);
}

// The case that catches an off-by-one in the wrap: repeatedly push a payload
// that is not a divisor of the capacity, so the write straddles the end of the
// backing array on most iterations, and verify the byte sequence never breaks.
static void test_wrap_integrity(void)
{
    printf("data survives thousands of wraps\n");
    Ring r;
    static uint8_t buf[256];
    ringInit(&r, buf, 256);

    uint8_t out[37], in[37];
    uint32_t wseq = 0, rseq = 0;
    int mismatch = -1;

    for (int iter = 0; iter < 5000 && mismatch < 0; iter++) {
        for (size_t i = 0; i < sizeof(in); i++) in[i] = (uint8_t)(wseq + i);
        size_t wrote = ringWrite(&r, in, sizeof(in));
        wseq += (uint32_t)wrote;

        size_t got = ringRead(&r, out, sizeof(out));
        for (size_t i = 0; i < got; i++) {
            if (out[i] != (uint8_t)(rseq + i)) { mismatch = iter; break; }
        }
        rseq += (uint32_t)got;
    }

    CHECK(mismatch < 0, "sequence broke on iteration %d", mismatch);
    CHECK(wseq == rseq, "wrote %u bytes but read %u", wseq, rseq);
    CHECK(wseq > 100000, "test did not actually move much data (%u bytes)", wseq);
}

// Mirrors how the app uses it: the network thread writes big irregular chunks,
// the audio thread drains smaller fixed ones, and the buffer sits mostly full.
static void test_producer_consumer_ratio(void)
{
    printf("survives a producer faster than the consumer\n");
    Ring r;
    static uint8_t buf[4096];
    ringInit(&r, buf, 4096);

    uint8_t chunk[1500], sink[512];
    memset(chunk, 0x5A, sizeof(chunk));

    size_t offered = 0, taken = 0, drained = 0;
    for (int i = 0; i < 200; i++) {
        offered += sizeof(chunk);
        taken   += ringWrite(&r, chunk, sizeof(chunk));
        drained += ringRead(&r, sink, sizeof(sink));
    }

    // The point is that back-pressure shows up as a short write, never as
    // corruption or a wedged ring.
    CHECK(taken < offered, "a slower consumer should have caused back-pressure");
    CHECK(taken == drained + ringUsed(&r),
          "bytes are unaccounted for: taken=%zu drained=%zu still buffered=%zu",
          taken, drained, ringUsed(&r));

    ringReset(&r);
    CHECK(ringUsed(&r) == 0, "reset should empty the ring");
}

int main(void)
{
    printf("== ring ==\n");
    test_rejects_bad_capacity();
    test_empty_and_full();
    test_short_counts();
    test_wrap_integrity();
    test_producer_consumer_ratio();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
