#include "ring.h"

#include <string.h>

// `volatile` (see ring.h) is only a compiler-ordering promise; it does not
// make the write of `head`/`tail` on one core show up in the right order to
// the other core. The producer is the network thread on core 1, the consumer
// is the decode thread on core 0 - two physical ARM11s, each with its own
// view of memory until a barrier forces a sync. __dmb() is libctru's real
// Data Memory Barrier (3ds/synchronization.h, pulled in by <3ds.h>); use it,
// don't hand-roll one. The host test build never defines __3DS__ and has
// only one thread, so the barrier there is nothing at all.
#ifdef __3DS__
#include <3ds.h>
#define RING_BARRIER() __dmb()
#else
#define RING_BARRIER() ((void)0)
#endif

bool ringInit(Ring *r, uint8_t *buf, size_t cap)
{
    if (!r || !buf || cap < 2) return false;
    if (cap & (cap - 1)) return false;  // not a power of two

    r->buf  = buf;
    r->cap  = cap;
    r->head = 0;
    r->tail = 0;
    return true;
}

void ringReset(Ring *r)
{
    r->head = 0;
    r->tail = 0;
}

size_t ringUsed(const Ring *r)
{
    // Unsigned wraparound makes this correct without a branch even when head
    // has wrapped past tail.
    return (r->head - r->tail) & (r->cap - 1);
}

size_t ringFree(const Ring *r)
{
    // The reserved byte: capacity is cap-1, not cap.
    return r->cap - 1 - ringUsed(r);
}

size_t ringWrite(Ring *r, const uint8_t *src, size_t len)
{
    size_t space = ringFree(r);
    if (len > space) len = space;
    if (len == 0) return 0;

    // `space` above was computed from `tail`, which the decode thread
    // publishes once it's done reading those bytes back out of `buf`. This
    // core needs to actually see that read as finished - not just see the
    // index change - before it starts overwriting the same memory.
    RING_BARRIER();

    size_t head = r->head;
    size_t mask = r->cap - 1;

    // Up to two copies: one to the end of the buffer, one wrapped to the front.
    size_t first = r->cap - head;
    if (first > len) first = len;
    memcpy(r->buf + head, src, first);
    if (len > first) memcpy(r->buf, src + first, len - first);

    // Publish the bytes before the index that says they're there. Skip this
    // and the decode thread on the other core could see the new `head` and
    // start reading a chunk boundary that hasn't actually landed in `buf`.
    RING_BARRIER();
    r->head = (head + len) & mask;
    return len;
}

size_t ringRead(Ring *r, uint8_t *dst, size_t len)
{
    size_t used = ringUsed(r);
    if (len > used) len = used;
    if (len == 0) return 0;

    // `used` above was computed from `head`, which the network thread
    // publishes after the memcpy that actually fills `buf`. Cross to that
    // core's write before trusting it, or this could read a chunk boundary
    // the write hasn't reached yet even though the index says it has.
    RING_BARRIER();

    size_t tail = r->tail;
    size_t mask = r->cap - 1;

    size_t first = r->cap - tail;
    if (first > len) first = len;
    memcpy(dst, r->buf + tail, first);
    if (len > first) memcpy(dst + first, r->buf, len - first);

    // Finish the read before publishing `tail`. The network thread treats an
    // advanced `tail` as permission to overwrite this same region, so it must
    // not see that permission before the read above is actually done.
    RING_BARRIER();
    r->tail = (tail + len) & mask;
    return len;
}
