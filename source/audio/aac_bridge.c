#include "aac_bridge.h"

#include <stdlib.h>
#include <string.h>

#include "aac/aacdec.h"

// Largest possible single decoded frame: AAC_MAX_NCHANS channels of
// AAC_MAX_NSAMPS samples each, 16-bit - TIMES TWO for SBR. Both macros come
// from the vendored aacdec.h (defaults 2 and 1024) - computed from them rather
// than hardcoded so a future change to either macro cannot silently desync this
// from the decoder's real output size.
//
// The SBR factor is not a safety margin, it is the decoder's own arithmetic.
// aacdec.c:186 sets the frame size as
//     outputSamps = nChans * AAC_MAX_NSAMPS * (sbrEnabled ? 2 : 1)
// so HE-AAC emits twice the samples of plain AAC-LC: a 24 kHz core comes out at
// 48 kHz. Without the factor of 2 this bound is 4096 bytes while stereo HE-AAC
// writes 8192, and since Helix is handed `scratch` as a raw pointer with no
// length, the overrun is a silent 4096-byte heap write - and the same undersized
// bound then lets the loop below memcpy past the CALLER's buffer too.
//
// MEASURED 2026-09-07 on a real AAC+ broadcast (mono, 24 kHz core): Helix
// reported outputSamps=2048, i.e. exactly 4096 bytes from ONE channel - the
// whole of the old bound. That this did not corrupt anything was luck: the
// capture happened to be mono. See the log entry for the stereo measurement.
//
// This was latent until this session. The directory query pinned codec=MP3, so
// no AAC station could be reached at all, let alone an AAC+ one.
#define MAX_FRAME_BYTES \
    ((size_t)AAC_MAX_NCHANS * (size_t)AAC_MAX_NSAMPS * 2u * sizeof(int16_t))

// Deliberate restatement of the requirement rather than of the definition. It
// looks tautological today because the macro above satisfies it exactly, and
// that is the point: the factor of 2 reads like padding to anyone who has not
// seen aacdec.c:186, so this is the thing that goes red if somebody tidies it
// away. Written in the decoder's own macro names so it stays true if the
// vendored build is ever reconfigured for more channels.
_Static_assert(MAX_FRAME_BYTES >=
                   (size_t)AAC_MAX_NCHANS * (size_t)AAC_MAX_NSAMPS * 2u * sizeof(int16_t),
               "scratch must hold Helix's LARGEST frame, and HE-AAC doubles it: "
               "aacdec.c computes outputSamps = nChans * AAC_MAX_NSAMPS * "
               "(sbrEnabled ? 2 : 1), so stereo AAC+ emits 8192 bytes");

// Bytes of ADTS header that must be staged before frame_length can be read.
#define ADTS_HEADER_BYTES 7u

// Room for one FEED_CHUNK-sized read (player.c uses 4096) plus a full worst
// case ADTS frame left over from the previous call (13-bit frame length
// field, max 8191 bytes) plus slack. Real Icecast AAC frames are far smaller
// than that ceiling - see the background notes on measured stations - so
// this is a deliberately generous bound, not a tuned one.
#define STAGE_CAP (4096u + 8192u + 4096u)

struct AacBridge {
    HAACDecoder h;

    uint8_t *stage;      // accumulates bytes across calls until a full frame exists
    size_t   stage_len;

    int16_t *scratch;    // one just-decoded frame, staged before being handed out

    long rate;           // 0 == "no frame decoded yet"
    int  channels;

    // A frame that changed format arrived while this call had already
    // produced older-format bytes. It sits fully decoded in `scratch`
    // (frozen - not overwritten until delivered) and is handed over as the
    // very first thing the NEXT aacBridgeDecode call does.
    bool   pending;
    size_t pending_bytes;
    long   pending_rate;
    int    pending_channels;
};

bool aacBridgeLooksLikeAdts(const uint8_t *buf, size_t len)
{
    if (len < 2) return false;
    // 12-bit syncword (0xFFF) with the 2 layer bits (always 0 for AAC) also
    // pinned down: buf[1] = 1111 V LL P, LL == 00. An MP3 frame sync instead
    // requires LL == 01 (Layer III), so the two can never both match - see
    // aac_bridge.h.
    return buf[0] == 0xFF && (buf[1] & 0xF6) == 0xF0;
}

AacBridge *aacBridgeNew(void)
{
    AacBridge *b = calloc(1, sizeof(*b));
    if (!b) return NULL;

    b->stage   = malloc(STAGE_CAP);
    b->scratch = malloc(MAX_FRAME_BYTES);
    b->h       = AACInitDecoder();

    if (!b->stage || !b->scratch || !b->h) {
        aacBridgeFree(b);
        return NULL;
    }
    return b;
}

void aacBridgeFree(AacBridge *b)
{
    if (!b) return;
    if (b->h) AACFreeDecoder(b->h);
    free(b->stage);
    free(b->scratch);
    free(b);
}

void aacBridgeReset(AacBridge *b)
{
    b->stage_len = 0;
    b->pending   = false;
    b->rate      = 0;
    b->channels  = 0;

    // A full free+reinit, not AACFlushCodec(). AACFlushCodec's own comment
    // says it clears "overlap buffers, etc." but measured behaviour (host
    // test_aac.c's test_reset_clears_state, comparing a checksum of the same
    // fixture decoded before and after a reset) shows it is not complete:
    // some per-channel state - almost certainly the PNS (perceptual noise
    // substitution) RNG seed, which FlushCodec's own source only documents
    // resetting "common state variables which change per-frame" - survives a
    // flush and measurably changes the decoded PCM on the next stream, even
    // though the frame count, sample rate and channel count all still come
    // out identical. A free+reinit is exactly what mpg123_close()+
    // mpg123_open_feed() already does on the MP3 side for the same reason
    // (see swPlayerPlay() in player.c) - it costs one small heap round trip
    // per station change, not a per-frame one, so there is no meaningful
    // performance argument for keeping the flush-only path once it is known
    // to leak state.
    if (b->h) {
        AACFreeDecoder(b->h);
        b->h = AACInitDecoder();
        // aacBridgeNew() already proved a fresh AACInitDecoder() succeeds on
        // this platform; a NULL here would mean the heap is exhausted mid-
        // session, which the existing decode calls already guard against
        // implicitly (Helix reads b->h through the same NULL-unsafe API
        // either way) - this is not a new failure mode, only a later point
        // where the pre-existing one could in principle surface.
    }
}

bool aacBridgeHasPending(const AacBridge *b)
{
    return b->pending;
}

// Length in bytes of the whole ADTS frame introduced by the header at `h`,
// including the header itself: a 13-bit field split across bytes 3, 4 and 5.
// The caller must have staged at least ADTS_HEADER_BYTES and have `h` pointing
// at a syncword.
static size_t adts_frame_length(const uint8_t *h)
{
    return ((size_t)(h[3] & 0x03) << 11) | ((size_t)h[4] << 3) |
           ((size_t)(h[5] & 0xE0) >> 5);
}

// Drops `n` bytes from the front of the staging buffer, sliding the rest down.
static void stage_drop(AacBridge *b, size_t n)
{
    if (n == 0) return;
    if (n >= b->stage_len) { b->stage_len = 0; return; }
    memmove(b->stage, b->stage + n, b->stage_len - n);
    b->stage_len -= n;
}

AacDecStatus aacBridgeDecode(AacBridge *b,
                             const uint8_t *in, size_t in_len,
                             int16_t *out, size_t out_cap, size_t *done,
                             long *rate, int *channels)
{
    *done = 0;

    // aacBridgeReset() re-creates the decoder on every station change, and an
    // AACInitDecoder() that fails there (heap exhausted mid-session) leaves
    // this NULL. Helix's API is not NULL-safe, so without this the next tune
    // is a crash instead of an error message. At startup the same failure is
    // already handled cleanly as SW_PLAYER_NO_MEMORY; this gives the mid-
    // session case the same dignity - player.c reads AACDEC_ERROR as a dead
    // stream and tells the user, rather than taking the console down.
    if (!b || !b->h) return AACDEC_ERROR;

    // Stage the new input FIRST, unconditionally - the header's contract is
    // that `in`/`in_len` are always fully consumed by this call, and that has
    // to hold even on the pending-delivery call below, or the bytes handed to
    // that call would simply be lost. `in_len` is always FEED_CHUNK-sized
    // (4096) coming from player.c, and STAGE_CAP has headroom for that plus a
    // full worst case leftover frame, so this should never need to drop
    // anything; the drop is only a last-resort guard against a pathological
    // stream that never produces a sync word, so a bug here fails safe
    // (resyncs) rather than corrupting memory.
    if (in_len) {
        if (b->stage_len + in_len > STAGE_CAP) {
            size_t overflow = (b->stage_len + in_len) - STAGE_CAP;
            stage_drop(b, overflow);
        }
        memcpy(b->stage + b->stage_len, in, in_len);
        b->stage_len += in_len;
    }

    // A format-changing frame was held back on a previous call because that
    // call's buffer already had older-format bytes in it. Hand it over now -
    // see the header comment for why this can't be merged into the loop
    // below, and player.c's fill_aac() for why callers must never invoke this
    // again into the same output buffer without checking aacBridgeHasPending
    // first (doing so would glue two formats into one DSP wavebuf).
    if (b->pending) {
        // Bounds-checked rather than trusting the caller. The header documents
        // that a pending delivery only ever happens into a full WBUF_BYTES
        // (32768) wavebuf, which is comfortably larger than the 4096-byte
        // maximum staged here, and player.c's fill_aac does honour that - but
        // the contract was enforced by a comment alone, so any future caller
        // that passed a smaller buffer would get a silent heap overflow rather
        // than an error. Refusing is safe: `pending` stays set, so the frame is
        // delivered on the next call with a big enough buffer instead of being
        // dropped.
        if (b->pending_bytes > out_cap) return AACDEC_OK;

        b->pending = false;
        memcpy(out, b->scratch, b->pending_bytes);
        *done = b->pending_bytes;
        b->rate     = b->pending_rate;
        b->channels = b->pending_channels;
        *rate     = b->rate;
        *channels = b->channels;
        return AACDEC_NEW_FORMAT;
    }

    for (;;) {
        if (out_cap - *done < MAX_FRAME_BYTES)
            return AACDEC_OK; // no room left for a whole frame this call

        int sync = AACFindSyncWord(b->stage, (int)b->stage_len);
        if (sync < 0) {
            // No syncword anywhere in what we have. Keep the last few bytes
            // in case a syncword straddles this call's boundary with the
            // next one; the rest is definitely not part of any frame.
            size_t keep = b->stage_len < 16 ? b->stage_len : 16;
            stage_drop(b, b->stage_len - keep);
            return *done ? AACDEC_OK : AACDEC_NEED_MORE;
        }
        stage_drop(b, (size_t)sync);

        // Never hand Helix a partial frame. AACDecode does NOT bounds-check the
        // frame body against the buffer it was given: UnpackADTSHeader only
        // fails if the 7-byte HEADER ran out, after which DecodeNoiselessData
        // reads the section data straight off the end. noiseless.c:137 is
        //     do { sectLenIncr = GetBits(bsi, sectLenBits);
        //          sectLen += sectLenIncr; } while (sectLenIncr == sectEscapeVal);
        // - an unbounded loop whose only exit is a value that is not the escape
        // value, so a bit reader running off the end and returning all-ones
        // spins forever. That is not a crash and not an error return: it is a
        // hung audio thread, i.e. a frozen console needing a power cycle.
        //
        // MEASURED 2026-09-07 on a real AAC+ broadcast: 99.9% CPU, no return,
        // and a SIGALRM backtrace reading AACDecode -> DecodeNoiselessData ->
        // GetBits. Attributed to truncation rather than to bad data by varying
        // the feed size - the hang followed the chunk boundary (feed 4096 hung
        // 21 bytes short at byte 135168; feed 2048 hung 26 bytes short at
        // 12288; feed 3000 hung 17 bytes short at 147000) over identical bytes.
        //
        // Staging short of a whole frame is the NORMAL case - it happens on
        // nearly every 4096-byte read - and it usually returns
        // ERR_AAC_INDATA_UNDERFLOW by luck of the bit patterns. The remainders
        // that hung were all under ~32 bytes, but a size threshold would be
        // guessing at a symptom; the frame length is stated in the header, so
        // wait for it.
        if (b->stage_len < ADTS_HEADER_BYTES)
            return *done ? AACDEC_OK : AACDEC_NEED_MORE;

        size_t frame_len = adts_frame_length(b->stage);

        // A frame_length that cannot be real means the syncword was a false
        // positive in compressed data, not a frame. Resync past it exactly as
        // the decode-error path below does. Without this the stream would
        // deadlock rather than hang: a length of 0 is never satisfiable, so the
        // staging buffer would fill and this loop would wait for it forever.
        if (frame_len < ADTS_HEADER_BYTES) {
            stage_drop(b, 1);
            continue;
        }

        // Not a truncated frame - just not all here YET. Same answer the
        // ERR_AAC_INDATA_UNDERFLOW branch below gives, for the same reason:
        // the next read will bring the rest. frame_len maxes out at 8191 (the
        // field is 13 bits) and STAGE_CAP is 16384, so this is always
        // eventually satisfiable and cannot wedge the stream.
        if (b->stage_len < frame_len)
            return *done ? AACDEC_OK : AACDEC_NEED_MORE;

        unsigned char *p    = b->stage;
        int            left = (int)b->stage_len;
        int err = AACDecode(b->h, &p, &left, b->scratch);

        if (err == ERR_AAC_INDATA_UNDERFLOW) {
            // The syncword is real but the frame it introduces is not fully
            // buffered yet - normal, not an error. Nothing consumed.
            return *done ? AACDEC_OK : AACDEC_NEED_MORE;
        }
        if (err != ERR_AAC_NONE) {
            // A genuine decode error on this frame. Resync by dropping just
            // the syncword byte we started from and looking for the next
            // one, rather than giving up on the whole stream over one bad
            // frame (e.g. a station briefly splicing in something else).
            if (b->stage_len == 0) return *done ? AACDEC_OK : AACDEC_ERROR;
            stage_drop(b, 1);
            continue;
        }

        // AACDecode advanced `p`/`left` past exactly the frame it consumed;
        // commit that to the staging buffer.
        //
        // The guard is not defensive clutter: this loop's only guarantee of
        // termination is that every pass either consumes staged bytes or
        // returns, and a success that consumed nothing satisfies neither. That
        // would be an infinite loop on the audio thread - the exact failure
        // this file already had once, above. Helix is not expected to do it
        // (every observed success consumed 224-310 bytes), but "not expected"
        // is what the truncation hang was too, so the loop is made structurally
        // unable to spin rather than trusted not to. Resyncing by one byte is
        // what the decode-error path does with an equally unusable frame.
        if ((size_t)left >= b->stage_len) {
            stage_drop(b, 1);
            continue;
        }
        stage_drop(b, b->stage_len - (size_t)left);

        AACFrameInfo info;
        AACGetLastFrameInfo(b->h, &info);
        size_t frame_bytes = (size_t)info.outputSamps * sizeof(int16_t);

        bool changed = (b->rate == 0) ||
                       info.sampRateOut != b->rate ||
                       info.nChans != b->channels;

        if (changed) {
            if (*done == 0) {
                // Stands alone in this call: adopt the new format now and
                // hand over this frame's own samples under it immediately -
                // there is no older-format data in `out` to keep separate.
                b->rate     = info.sampRateOut;
                b->channels = info.nChans;
                memcpy(out, b->scratch, frame_bytes);
                *done = frame_bytes;
                *rate     = b->rate;
                *channels = b->channels;
                return AACDEC_NEW_FORMAT;
            }
            // Older-format bytes are already queued in `out` this call; this
            // frame must start its own buffer instead of being mixed in.
            // Hold it (already sitting in `scratch`) for the next call.
            b->pending          = true;
            b->pending_bytes    = frame_bytes;
            b->pending_rate     = info.sampRateOut;
            b->pending_channels = info.nChans;
            return AACDEC_OK;
        }

        memcpy((uint8_t *)out + *done, b->scratch, frame_bytes);
        *done += frame_bytes;
    }
}
