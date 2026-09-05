#pragma once

// A single-producer / single-consumer byte ring buffer.
//
// This is what stands between WiFi and the speaker. The network thread writes
// compressed audio into it as fast as the connection delivers; the audio thread
// drains it at exactly the rate the decoder consumes. Sizing it generously is
// the whole point - a few seconds of buffered stream is what turns a WiFi
// stall into something the listener never hears.
//
// Correctness rests on there being exactly ONE writer thread and exactly ONE
// reader thread. Under that rule no lock is needed: the writer only ever
// advances `head`, the reader only ever advances `tail`, and each reads the
// other's index once per operation. `volatile` keeps the compiler from caching
// those reads across the loop.
//
// `cap` must be a power of two so the wrap is a mask rather than a modulo.
// One byte is always left unused, which is what makes full and empty
// distinguishable without a separate count.
//
// No 3DS headers here on purpose - it builds and tests on the host.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t        *buf;
    size_t          cap;   // power of two
    volatile size_t head;  // next write position - owned by the producer
    volatile size_t tail;  // next read position  - owned by the consumer
} Ring;

// `cap` must be a power of two. Returns false if it is not, or if buf is null.
bool   ringInit(Ring *r, uint8_t *buf, size_t cap);

// Discards everything buffered. Only safe when neither thread is running -
// used when retuning to a different station.
void   ringReset(Ring *r);

size_t ringUsed(const Ring *r);
size_t ringFree(const Ring *r);

// Writes as much of `src` as fits. Returns how many bytes were taken, which
// may be 0 or a short count when the buffer is full. Producer thread only.
size_t ringWrite(Ring *r, const uint8_t *src, size_t len);

// Reads up to `len` bytes. Returns how many were available, which may be 0 or
// a short count. Consumer thread only.
size_t ringRead(Ring *r, uint8_t *dst, size_t len);
