// A stub http.h that shadows the real one, so directory.c can be compiled and
// run on the host. Used only by the `mirrors` target in tests/Makefile.
//
// Why this exists: the real source/net/http.h pulls in tcp.h -> mbedtls, which
// is why no host suite had ever compiled directory.c and why its mirror-retry
// loop went unproven for the life of the project. Nothing in that loop cares
// about TLS - it cares about what swHttpGetText returned - so the network is
// the ONLY thing replaced, and test_mirrors.c replaces it with something that
// COUNTS CALLS, which is the quantity actually under test. The loop itself,
// result_rank(), host_pref and the real swDirParseJson are all shipping code.
//
// How it wins over the real header: `#include "http.h"` is resolved relative to
// the INCLUDING FILE's directory first, before any -I path, so a -I pointing
// here would lose. The Makefile therefore copies directory.c into the build
// directory next to a copy of this file. The copy is remade on every run, so it
// cannot drift from the tree.
//
// Keep the three declarations below in step with source/net/http.h. They are
// the entire surface directory.c uses; if it starts calling something else, the
// `mirrors` target fails to link, which is the right kind of failure.
#pragma once

#include <stddef.h>

#define SW_URL_MAX 2048

typedef struct SwHttp { int unused; } SwHttp;

int swHttpGetText(const char *url, char *buf, size_t cap);
int swHttpGetTextBoundedH(SwHttp *h, const char *url, char *buf, size_t cap,
                          size_t head_cap, size_t line_cap);
