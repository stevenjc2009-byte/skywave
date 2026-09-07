#include "icy.h"

#include <string.h>

void icyInit(IcyDemux *d, int metaint)
{
    memset(d, 0, sizeof(*d));
    d->metaint    = metaint > 0 ? metaint : 0;
    d->state      = ICY_AUDIO;
    d->audio_left = d->metaint;
}

bool icyParseTitle(const char *meta, char *dst, size_t cap)
{
    if (cap == 0) return false;

    const char *key = "StreamTitle='";
    const char *p = strstr(meta, key);
    if (!p) return false;
    p += strlen(key);

    // The value ends at the first `';` - a bare quote is not enough, because
    // apostrophes turn up in track titles constantly ("Don't Stop Me Now").
    const char *end = strstr(p, "';");
    if (!end) {
        end = p + strlen(p);
    }

    size_t n = (size_t)(end - p);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, p, n);
    dst[n] = 0;
    return true;
}

size_t icyFeed(IcyDemux *d, const uint8_t *in, size_t in_len, uint8_t *out)
{
    // No metadata interleaved: the body is already pure audio.
    if (d->metaint <= 0) {
        memcpy(out, in, in_len);
        return in_len;
    }

    size_t consumed = 0, produced = 0;

    while (consumed < in_len) {
        size_t avail = in_len - consumed;

        switch (d->state) {
        case ICY_AUDIO: {
            size_t n = (size_t)d->audio_left < avail ? (size_t)d->audio_left : avail;
            memcpy(out + produced, in + consumed, n);
            produced      += n;
            consumed      += n;
            d->audio_left -= (int)n;
            if (d->audio_left == 0) d->state = ICY_LEN;
            break;
        }

        case ICY_LEN: {
            int len = in[consumed++];
            if (len == 0) {
                // The overwhelmingly common case - an empty block, emitted on
                // schedule whether or not anything changed.
                d->state      = ICY_AUDIO;
                d->audio_left = d->metaint;
            } else {
                d->meta_left = len * 16;
                d->meta_fill = 0;
                d->state     = ICY_META;
            }
            break;
        }

        case ICY_META: {
            size_t n = (size_t)d->meta_left < avail ? (size_t)d->meta_left : avail;
            // Bounded by construction (meta_left <= 255*16 == ICY_META_MAX),
            // but a stream is untrusted input and this is cheap.
            size_t room = ICY_META_MAX - (size_t)d->meta_fill;
            size_t copy = n < room ? n : room;
            memcpy(d->meta + d->meta_fill, in + consumed, copy);
            d->meta_fill += (int)copy;

            consumed     += n;
            d->meta_left -= (int)n;

            if (d->meta_left == 0) {
                d->meta[d->meta_fill] = 0;

                char t[ICY_TITLE_MAX];
                if (icyParseTitle(d->meta, t, sizeof(t))) {
                    // Only flag a real change. Servers repeat the same title in
                    // every block, and a UI that redraws on each one flickers.
                    if (strcmp(t, d->title) != 0) {
                        // Only up to the terminator. Copying sizeof(t) dragged
                        // the whole 256-byte stack buffer across, uninitialised
                        // tail and all - harmless, since both buffers are the
                        // same size and every reader stops at the NUL, but it
                        // copies rubbish the destination has no reason to hold.
                        memcpy(d->title, t, strlen(t) + 1);
                        d->title_changed = true;
                    }
                }

                d->state      = ICY_AUDIO;
                d->audio_left = d->metaint;
            }
            break;
        }
        }
    }

    return produced;
}

bool icyTakeTitle(IcyDemux *d, char *dst, size_t cap)
{
    if (!d->title_changed || cap == 0) return false;

    size_t n = strlen(d->title);
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, d->title, n);
    dst[n] = 0;

    d->title_changed = false;
    return true;
}
