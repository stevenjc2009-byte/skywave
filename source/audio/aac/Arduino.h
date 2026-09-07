#pragma once

// SKYWAVE-AUTHORED COMPATIBILITY SHIM - NOT part of the vendored Helix/RPSL
// source, and NOT the real Arduino core header.
//
// aaccommon.h (RealNetworks, RPSL-licensed, unmodified - see LICENSE-RPSL-1.0.txt
// and README.md in this directory) does `#include <Arduino.h>` unconditionally.
// Because this file uses angle brackets, and because the Makefile puts
// source/audio/aac on the include path (for the decoder's own public headers),
// the preprocessor finds THIS file rather than failing the build - without
// editing a single byte of the RPSL-licensed source.
//
// The only thing aaccommon.h actually wants from the real Arduino.h is the
// AVR/ESP "program memory" macro family (PROGMEM et al.), used there to place
// Helix's lookup tables in flash separately from RAM on chips with a split
// address space. The 3DS has no such split - ROM and RAM are both flatly
// addressable - so every macro below is either empty or a plain dereference.
// See source/audio/aac/README.md, "Local changes" for why this exists.
//
// (The NuttX/Vela port of this same decoder, open-vela/external_libhelix-aac,
// solves the identical problem the identical way: a local Arduino.h shim.)
//
// stdio.h is included below for the same reason: sbr.c calls printf() (an
// OOM diagnostic in InitSBRPre/raac_InitSBR) without including <stdio.h>
// itself. On the real Arduino/ESP8266 core this compiles anyway because the
// genuine Arduino.h transitively pulls in stdio through its own core headers
// - this shim has to do that explicitly instead, or sbr.c fails to compile
// with an implicit-declaration error (found by actually building this host
// test, not by inspection).

#include <stdint.h>
#include <stdio.h>

#define PROGMEM
#define PSTR(s) (s)
#define memcpy_P memcpy
#define sprintf_P sprintf
#define snprintf_P snprintf
#define strcpy_P strcpy
#define strncpy_P strncpy

#ifndef ICACHE_RODATA_ATTR
#define ICACHE_RODATA_ATTR
#endif

#ifndef PGM_P
#define PGM_P const char *
#endif

#ifndef PGM_VOID_P
#define PGM_VOID_P const void *
#endif

#ifdef __cplusplus
#define pgm_read_byte(addr)  (*reinterpret_cast<const uint8_t*>(addr))
#define pgm_read_word(addr)  (*reinterpret_cast<const uint16_t*>(addr))
#define pgm_read_dword(addr) (*reinterpret_cast<const uint32_t*>(addr))
#else
#define pgm_read_byte(addr)  (*(const uint8_t*)(addr))
#define pgm_read_word(addr)  (*(const uint16_t*)(addr))
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#endif

#define pgm_read_byte_near(addr)  pgm_read_byte(addr)
#define pgm_read_word_near(addr)  pgm_read_word(addr)
#define pgm_read_dword_near(addr) pgm_read_dword(addr)
#define pgm_read_byte_far(addr)   pgm_read_byte(addr)
#define pgm_read_word_far(addr)   pgm_read_word(addr)
#define pgm_read_dword_far(addr)  pgm_read_dword(addr)
