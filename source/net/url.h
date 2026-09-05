#pragma once

// URL surgery, kept free of any 3DS header so it can be proven on a PC.
//
// This is one function today, and it is here rather than inside http.c for one
// reason: it rewrites the address the app is about to connect to, and getting
// it subtly wrong - an off-by-one on the scheme, a truncated query string -
// would show up as "could not connect" on a console with no log, which is the
// hardest possible place to debug it.

#include <stdbool.h>
#include <stddef.h>

// Rewrites `https://rest` into `http://rest` in `out`.
//
// Returns false, leaving `out` untouched, if `in` is not an https URL or if the
// result would not fit in `cap` (including the terminator). The scheme match is
// case-insensitive: RFC 3986 says schemes are, and directory data really does
// contain "HTTPS://".
//
// Why the app ever wants this: see the comment on begin_or_plain in http.c. In
// short, the console's TLS is fixed at a level most streaming hosts no longer
// accept, and plain http is the only way back to a large share of them.
bool swUrlToPlainHttp(const char *in, char *out, size_t cap);
