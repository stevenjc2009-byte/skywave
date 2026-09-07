#pragma once

// SKYWAVE-AUTHORED COMPATIBILITY SHIM - NOT part of the vendored Helix/RPSL
// source, and NOT the real Helix hlxclib.
//
// buffers.c and sbr.c (RealNetworks, RPSL-licensed, unmodified) each do:
//
//   #if defined(USE_DEFAULT_STDLIB) || defined(ARDUINO)
//   #include <stdlib.h>
//   #else
//   #include "hlxclib/stdlib.h"
//   #endif
//
// USE_DEFAULT_STDLIB is exactly the knob Helix itself provides for "skip the
// cross-platform hlxclib abstraction layer, just use the real C library" -
// aacdec.h even self-defines it as a fallback (`#ifndef USE_DEFAULT_STDLIB
// #define USE_DEFAULT_STDLIB#endif`). But that self-define happens too late:
// it only takes effect once aacdec.h is actually included, which in buffers.c
// and sbr.c is AFTER this #if already ran (via coder.h, included further
// down each file), so by the time these two files check the macro it is not
// defined yet in either of them.
//
// The vendored source (earlephilhower/ESP8266Audio) never has to deal with
// this: it is Arduino-only, so `defined(ARDUINO)` is always true there and
// this branch is taken every time - the hlxclib/ directory referenced by the
// #else doesn't even exist in that repo (checked at the pinned commit; see
// source/audio/aac/README.md). Skywave is not Arduino, so neither macro is
// true here, and the #include "hlxclib/stdlib.h" branch is the one that
// actually runs - hence this file has to exist.
//
// Rather than defining USE_DEFAULT_STDLIB or ARDUINO as a build-wide macro
// (which would also change behaviour in other files gated on the same
// macros, in ways not audited here), this supplies the missing header
// directly, as a pure pass-through to the real C library - which is exactly
// what USE_DEFAULT_STDLIB would have made these two files do anyway. Quote-
// form #include resolves this against buffers.c/sbr.c's own directory
// (source/audio/aac/), so no Makefile change is needed beyond what already
// exists for Arduino.h/pgmspace.h.

#include <stdlib.h>
