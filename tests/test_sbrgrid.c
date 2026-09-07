// Host tests for the SBR grid parser (source/audio/aac/sbrside.c), the part of
// the vendored RealNetworks Helix decoder (RPSL-licensed - see
// source/audio/aac/README.md) that reads the HE-AAC envelope layout straight
// off the wire.
//
// Why this suite exists. UnpackSBRGrid's FIXFIX case computed
// `numEnv = (1 << numEnvRaw)` from a 2-bit field, so numEnv reached 8 against a
// MAX_NUM_ENV of 5, overrunning both a struct field (freqRes) and a STACK array
// (relBordLead). The ASSERT stating the real contract expands to nothing on GCC
// (sbr.h:56), so nothing enforced it. VARVAR had the same latent problem:
// numEnv = numRelBorder0 + numRelBorder1 + 1 reaches 7. Two bits of broadcast
// data were enough, on a code path that is live for every AAC+ station.
//
// The invariant under test is deliberately narrow and total: WHATEVER bits
// arrive, 1 <= numEnv <= MAX_NUM_ENV. That framing matters, because it means
// the test cannot be wrong about the bitstream layout - every 16-bit prefix is
// swept below, valid or not, and the invariant must hold for all of them. A
// test that had to construct a "correct" SBR grid could be quietly testing
// nothing if a field offset were mis-transcribed.
//
// UnpackSBRGrid is static, so this file includes the .c to reach it. The
// Makefile therefore links the vendored objects WITHOUT aacvendor_sbrside.o,
// which would duplicate every symbol here.

#include <stdio.h>
#include <string.h>

#include "sbrside.c"

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

// Comfortably more than the ~50 bits a grid parse reads. GetBits does not
// bounds-check either, so a short buffer here would trip the sanitizer on the
// READ side and report a problem this suite is not about.
#define GRID_BYTES 64

// Parses one grid from `prefix` (big-endian, MSB first, as GetBits reads) and
// returns numEnv. Everything after the prefix is zero.
static int parse_grid(unsigned int prefix)
{
    unsigned char bits[GRID_BYTES];
    BitStreamInfo bsi;
    SBRHeader hdr;
    SBRGrid grid;

    memset(bits, 0, sizeof(bits));
    bits[0] = (unsigned char)((prefix >> 8) & 0xFF);
    bits[1] = (unsigned char)(prefix & 0xFF);

    memset(&hdr, 0, sizeof(hdr));
    memset(&grid, 0, sizeof(grid));

    SetBitstreamPointer(&bsi, (int)sizeof(bits), bits);
    UnpackSBRGrid(&bsi, &hdr, &grid);
    return grid.numEnv;
}

// The whole point: sweep every 16-bit prefix, which covers all four frame
// classes and every combination of the small count fields that feed numEnv.
// Under ASan this also exercises the stores that used to overflow; without a
// sanitizer the value assertion still goes red, which is what makes this
// meaningful on the MinGW build too.
static void test_numenv_is_always_bounded(void)
{
    int over = 0, under = 0, worst = 0;
    unsigned int worst_prefix = 0;
    unsigned int p;

    for (p = 0; p <= 0xFFFFu; p++) {
        int n = parse_grid(p);

        if (n > worst) { worst = n; worst_prefix = p; }
        if (n > MAX_NUM_ENV) over++;
        if (n < 1)          under++;
    }

    CHECK(over == 0,
          "%d of 65536 bit patterns produced numEnv > MAX_NUM_ENV (%d); "
          "worst was %d from prefix 0x%04X",
          over, MAX_NUM_ENV, worst, worst_prefix);
    CHECK(under == 0, "%d bit patterns produced numEnv < 1", under);
    CHECK(worst <= MAX_NUM_ENV, "highest numEnv seen was %d, bound is %d",
          worst, MAX_NUM_ENV);
}

// The two specific patterns that were measured overflowing before the fix.
// Exact expected values, not just "in bounds": FIXFIX clamps to 4 rather than
// to MAX_NUM_ENV because the border arithmetic there divides NUM_TIME_SLOTS by
// numEnv and takes numEnv >> 1, both of which assume a power of two. A change
// that clamped FIXFIX to 5 would be in bounds and still wrong.
static void test_the_two_measured_overflows(void)
{
    // frameClass=00 (FIXFIX), numEnvRaw=11 -> asked for 8. freqRes0=0.
    // 0b0011 0000 ... = 0x3000
    CHECK(parse_grid(0x3000u) == 4,
          "FIXFIX with numEnvRaw=3 asked for 8 envelopes, got %d, expected 4",
          parse_grid(0x3000u));

    // frameClass=11 (VARVAR), absBorder0=00, absBorder1=00,
    // numRelBorder0=11, numRelBorder1=11 -> asked for 3+3+1 = 7.
    // 0b1100 0011 1100 0000 = 0xC3C0
    CHECK(parse_grid(0xC3C0u) == MAX_NUM_ENV,
          "VARVAR with both counts at 3 asked for 7 envelopes, got %d, "
          "expected %d", parse_grid(0xC3C0u), MAX_NUM_ENV);

    // The legal FIXFIX values must be untouched by the clamp.
    CHECK(parse_grid(0x0000u) == 1, "FIXFIX numEnvRaw=0 should stay 1, got %d",
          parse_grid(0x0000u));
    CHECK(parse_grid(0x1000u) == 2, "FIXFIX numEnvRaw=1 should stay 2, got %d",
          parse_grid(0x1000u));
    CHECK(parse_grid(0x2000u) == 4, "FIXFIX numEnvRaw=2 should stay 4, got %d",
          parse_grid(0x2000u));
}

int main(void)
{
    printf("== sbrgrid ==\n");
    test_numenv_is_always_bounded();
    test_the_two_measured_overflows();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
