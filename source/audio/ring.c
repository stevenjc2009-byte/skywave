#include "ring.h"

#include <string.h>

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

    size_t head = r->head;
    size_t mask = r->cap - 1;

    // Up to two copies: one to the end of the buffer, one wrapped to the front.
    size_t first = r->cap - head;
    if (first > len) first = len;
    memcpy(r->buf + head, src, first);
    if (len > first) memcpy(r->buf, src + first, len - first);

    r->head = (head + len) & mask;
    return len;
}

size_t ringRead(Ring *r, uint8_t *dst, size_t len)
{
    size_t used = ringUsed(r);
    if (len > used) len = used;
    if (len == 0) return 0;

    size_t tail = r->tail;
    size_t mask = r->cap - 1;

    size_t first = r->cap - tail;
    if (first > len) first = len;
    memcpy(dst, r->buf + tail, first);
    if (len > first) memcpy(dst + first, r->buf, len - first);

    r->tail = (tail + len) & mask;
    return len;
}
