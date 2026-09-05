#pragma once

// SHOUTcast/Icecast in-band metadata demultiplexer.
//
// When a request carries `Icy-MetaData: 1`, the server may answer with an
// `icy-metaint: N` header. That does NOT mean the body is a clean MP3 stream
// any more: every N bytes of audio the server splices in a metadata block, and
// a decoder fed those bytes verbatim will hear them as noise.
//
// The block format, verified against a live capture of Classic FM on
// 2026-09-05 (`icy-metaint: 8000`):
//
//   [ N bytes audio ][ len ][ len*16 bytes of text ][ N bytes audio ][ len ]...
//
// `len` is a single byte holding the block length divided by 16, so a metadata
// block is at most 255*16 = 4080 bytes. `len == 0` is the common case and
// means the block is empty - servers emit one every N bytes regardless of
// whether the title changed. The text is ASCII of the shape
// `StreamTitle='...';StreamUrl='...';`.
//
// The audio either side of a block is contiguous: removing the block rejoins
// an MP3 frame that was split across it. That is why this has to strip the
// bytes rather than resynchronise around them.
//
// This file is deliberately free of 3DS headers so it can be built and tested
// on the host against a captured stream.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define ICY_META_MAX  (255 * 16)  // largest block a one-byte length can describe
#define ICY_TITLE_MAX 256

typedef enum {
    ICY_AUDIO = 0,  // counting down to the next metadata block
    ICY_LEN,        // the next byte is the length byte
    ICY_META        // inside a metadata block
} IcyState;

typedef struct {
    int      metaint;        // 0 means the stream carries no metadata at all
    IcyState state;
    int      audio_left;     // audio bytes still to pass through before the next block
    int      meta_left;      // metadata bytes still to swallow
    int      meta_fill;
    char     meta[ICY_META_MAX + 1];

    char     title[ICY_TITLE_MAX];  // last StreamTitle seen; empty until one arrives
    bool     title_changed;         // set on change, cleared by icyTakeTitle()
} IcyDemux;

// `metaint` is the value of the icy-metaint response header, or 0 if absent.
void icyInit(IcyDemux *d, int metaint);

// Consumes `in_len` bytes and writes the audio-only bytes to `out`.
//
// `out` never needs to be larger than `in_len`: stripping can only ever remove
// bytes, never add them. Returns the number of audio bytes written.
size_t icyFeed(IcyDemux *d, const uint8_t *in, size_t in_len, uint8_t *out);

// Copies the current title out if it changed since the last call, and clears
// the changed flag. Returns false (and leaves `dst` untouched) if it did not.
bool icyTakeTitle(IcyDemux *d, char *dst, size_t cap);

// Extracts the value of `StreamTitle='...'` from a raw metadata block.
// Exposed for testing. Returns false if the key is absent.
bool icyParseTitle(const char *meta, char *dst, size_t cap);
