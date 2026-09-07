#pragma once

// Thin wrapper around the vendored Helix AAC decoder (source/audio/aac/,
// RealNetworks, RPSL-licensed - see source/audio/aac/README.md). This is
// Skywave's own code, MIT-licensed like the rest of the app; it exists so
// player.c never has to touch Helix's own headers or call convention
// directly, and so the two codecs plug into player.c's fill() the same way.
//
// NAMING NOTE: this is deliberately NOT named aacdec.c/aacdec.h. The vendored
// library already owns those two filenames (source/audio/aac/aacdec.c and
// aacdec.h). The 3DS Makefile globs every SOURCES directory into one flat
// build/ directory keyed by basename only (see the top-level Makefile's
// comment on CFILES/OFILES_SRC) - two files sharing a name in different
// SOURCES directories silently collide into the same build/aacdec.o, and one
// of them stops being compiled at all with no error. Calling this file
// aac_bridge.c/.h avoids that entirely.
//
// Call shape mirrors mpg123_decode()/mpg123_getformat() as used in player.c's
// fill(), on purpose, so the two codec branches read the same way:
//
//   AacDecStatus st = aacBridgeDecode(dec, feed, got, out, cap, &done, &rate, &ch);
//   if (st == AACDEC_NEW_FORMAT) { set_format(rate, ch); if (done) return done; }
//   if (st == AACDEC_ERROR) { ... treat like a dead stream ... }
//
// Difference from mpg123's convention, and why: mpg123 can announce a format
// change with zero new samples (it discovers the header before it has decoded
// anything under it) and expects the caller to call it again to actually get
// samples. Helix decodes a whole ADTS frame - header and PCM - atomically in
// one AACDecode() call, so there is no such thing as a formatless
// announcement here: AACDEC_NEW_FORMAT always carries that frame's own PCM
// in `*out`/`*done`, already in the new rate/channels. If a format change is
// detected after this call already produced some OLD-format bytes, that
// frame is held back (not mixed into the same wavebuf) and delivered first
// thing on the following call instead - see aac_bridge.c for the exact
// bookkeeping.

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct AacBridge AacBridge;

typedef enum {
    AACDEC_OK,          // *done bytes written (possibly 0), in the current format
    AACDEC_NEED_MORE,   // *done == 0; no complete ADTS frame available yet - feed more input
    AACDEC_NEW_FORMAT,  // *done bytes written, all already in the NEW *rate/*channels
    AACDEC_ERROR        // *done == 0; could not resynchronise at all - treat as a dead stream
} AacDecStatus;

// Allocates the decoder and its buffers on the heap. Returns NULL on
// allocation failure (mirrors mpg123_new returning NULL - swPlayerInit's
// existing `goto nomem` handles that the same way for either codec).
AacBridge *aacBridgeNew(void);
void       aacBridgeFree(AacBridge *b);

// Drops all buffered bytes and decoder state and starts fresh. Call this
// exactly where swPlayerPlay() currently does mpg123_close()+mpg123_open_feed
// - once per station change, before the first byte of the new stream arrives.
void aacBridgeReset(AacBridge *b);

// Sniffs whether `buf` (>= 2 bytes) begins with an ADTS AAC syncword: 0xFF
// followed by a byte whose top 4 bits are 1111 and layer bits (bits 2-1) are
// 00. This is deliberately checked to be mutually exclusive with an MPEG
// Layer III (MP3) frame sync, which requires those same layer bits to be 01
// - so a buffer can never sniff as both. See player.c's codec-detection
// comment for the full reasoning and why sniffing was chosen over trusting
// the HTTP Content-Type header.
bool aacBridgeLooksLikeAdts(const uint8_t *buf, size_t len);

// True when a decoded, format-changing frame is sitting inside `b` waiting to
// be delivered as the next AACDEC_NEW_FORMAT. A caller that accumulates
// several aacBridgeDecode() calls into one output buffer (player.c's
// fill_aac(), mirroring fill_mp3()'s use of mpg123_decode()) MUST check this
// before making another call into the SAME buffer: once true, the next call
// delivers the pending frame under the new format regardless of what new
// input is passed in, and appending that to bytes already collected under
// the OLD format would glue two formats into one DSP wavebuf. Stop and start
// a fresh output buffer instead - see player.c.
bool aacBridgeHasPending(const AacBridge *b);

// Feeds `in_len` new bytes (from the ring, same FEED_CHUNK-sized reads player
// c already does for mpg123) and decodes as many complete ADTS frames as fit
// into `out_cap` bytes of `out` (s16, interleaved). `in`/`in_len` are always
// considered fully consumed by this call - any partial trailing frame is
// retained inside `b`, exactly like mpg123's own internal feed buffer, so the
// caller never needs to hold bytes back itself.
AacDecStatus aacBridgeDecode(AacBridge *b,
                             const uint8_t *in, size_t in_len,
                             int16_t *out, size_t out_cap, size_t *done,
                             long *rate, int *channels);
