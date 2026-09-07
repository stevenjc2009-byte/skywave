# libhelix-aac (third-party, vendored)

This directory contains RealNetworks' fixed-point HE-AAC ("Helix") decoder.
It is **not** Skywave code and is **not** covered by Skywave's MIT licence
(see `/LICENSE` at the project root). Its own licence is in
`LICENSE-RPSL-1.0.txt` in this directory (RealNetworks Public Source
License v1.0 - "RPSL"). Skywave's MIT licence makes no claim over the
contents of this directory.

## Provenance

- Upstream: https://github.com/earlephilhower/ESP8266Audio
  path `src/libhelix-aac/`
- Commit pinned: `74fc1f09bbba5e5c5450b445452ba64ef2d8bbad` (fetched 2026-09-06)
- Original code: RealNetworks' Helix DNA HE-AAC decoder, 2005
  (`aacdec.c` etc. carry the original `$Id$` tags and RPSL header block).
  ESP8266Audio is itself a redistribution of this decoder with the ARM/x86
  assembly optimisations left in place but *disabled for our target* (see
  "Local changes" below) and Arduino-flavoured PROGMEM macros layered on
  top via `#include <Arduino.h>` / `<pgmspace.h>` in `aaccommon.h`.
- 36 files came from that commit: 28 `.c`, 7 `.h` (`aaccommon.h`,
  `aacdec.h`, `assembly.h`, `bitstream.h`, `coder.h`, `sbr.h`,
  `statname.h`), and the upstream `readme.txt`. **35 of them are
  byte-identical to upstream.** The exception is `assembly.h`, which
  carries two local guard edits - see "Local changes" below.
- This directory now holds 41 files in total: those 36, plus
  `LICENSE-RPSL-1.0.txt`, this `README.md`, the `Arduino.h` and
  `pgmspace.h` shims, and `hlxclib/stdlib.h`. The last three are
  Skywave-authored compatibility headers and are **not** RPSL code.
  Upstream's layout was flat; the `hlxclib/` subdirectory is ours.

Why this source and not another candidate:
- The original RealNetworks Helix DNA client release and Rockbox's tree
  were considered; this ESP8266Audio copy was chosen because it is a
  known-working, currently-maintained, plain-C redistribution used in
  production on other 32-bit embedded ARM/Xtensa targets (Arduino/ESP32,
  and forks of it on RP2040 - real ARM Cortex-M0 hardware), and because
  its `assembly.h` already has the real ARM inline-assembly branch
  deliberately disabled (see below) rather than live and requiring us to
  disable it ourselves.
- pjsip's `third_party` tree does not carry a libhelix-aac copy (checked:
  its third-party libs are g7221/gsm/ilbc/speex/srtp/webrtc/etc., no AAC
  decoder).
- `pschatzmann/arduino-libhelix` wraps this same ESP8266Audio source in a
  C++/Arduino Stream API we would have had to strip back out, so the
  ESP8266Audio source was taken directly instead.

## Local changes (on top of the pinned commit)

Four changes were made, all narrowly scoped, all required to build under
devkitARM (and, for one of them, under this project's MinGW-w64 host test
compiler) rather than Arduino. Only the first two touch RPSL-licensed
code at all; the other two are Skywave-authored files added alongside it.

1. **`assembly.h` (line 558)**: the generic (non-assembly) C fallback
   branch's `#elif` condition was extended to also match our target.
   Before:
   ```c
   #elif defined(ARDUINO) || defined(__GNUC__) && (defined(__mips__) || defined(__MIPS__)) || defined(__GNUC__) && (defined(__powerpc__) || defined(__POWERPC__)) || (defined (_SOLARIS) && !defined (__GNUC__) && !defined (_SOLARISX86))
   ```
   After (added the trailing `||` clause only):
   ```c
   #elif defined(ARDUINO) || defined(__GNUC__) && (defined(__mips__) || defined(__MIPS__)) || defined(__GNUC__) && (defined(__powerpc__) || defined(__POWERPC__)) || (defined (_SOLARIS) && !defined (__GNUC__) && !defined (_SOLARISX86)) || (defined(__GNUC__) && defined(__arm__) && defined(__3DS__))
   ```
   Reasoning: this file's *real* ARM GNU-C branch
   (`#elif defined(__GNUC__) && defined(XXXX__arm__)`) is written with a
   deliberately misspelled macro (`XXXX__arm__` is never defined by any
   compiler), so it can never be selected - upstream already ships with
   ARM inline assembly permanently switched off. But devkitARM
   (`__GNUC__` + real `__arm__`) does not match *any* of the remaining
   branches either, so without this change the file hits
   `#error Unsupported platform in assembly.h`. The added clause is
   scoped to `__3DS__` (defined by this project's own Makefile,
   `-D__3DS__`) so it changes behaviour for this target only and leaves
   every other platform's branch exactly as upstream shipped it. This
   selects the same portable `MULSHIFT32`/`CLIPTOSHORT`/`CLZ`/`MADD64`
   C implementation (64-bit `long long` arithmetic, no inline asm) that
   upstream already uses for MIPS, PowerPC and Arduino-Xtensa targets -
   i.e. no new code, just routing devkitARM into an existing, already
   portable path.

2. **`assembly.h` (line 71)**: `&& !defined(__GNUC__)` was added to the
   MSVC inline-assembly branch's condition. That branch is selected on
   `_WIN32`, and MinGW-w64 GCC - which is what builds this project's host
   test suite on Windows - defines `_WIN32` too, so it was being routed
   into `__asm { mov eax, ... }` blocks GCC cannot parse. Confirmed with
   `gcc -dM -E` showing both `_WIN32` and `__MINGW32__` defined; found by
   an actual compile failure, not by inspection. Real MSVC still takes the
   branch exactly as upstream shipped it.

3. **`Arduino.h` / `pgmspace.h` shims added** (this directory only -
   these are Skywave-authored compatibility headers, not RPSL code, and
   are NOT modifications to any Helix file): `aaccommon.h` does
   `#include <Arduino.h>` and `#include <pgmspace.h>` unconditionally.
   Rather than edit that RPSL-licensed file, two tiny local headers of
   those exact names were added to this same directory so the angle-
   bracket `#include` resolves to them once `source/audio/aac` is on the
   include path (see the Makefile change). `Arduino.h` defines `PROGMEM`,
   `PSTR`, `pgm_read_*` and friends as plain no-ops/direct dereferences
   (the 3DS has no separate program-memory address space - ROM and RAM
   are both flatly addressable - so there is nothing for these macros to
   do). `pgmspace.h` is empty. This is the same technique the NuttX/Vela
   RTOS port of this decoder (`open-vela/external_libhelix-aac`) uses for
   its own non-Arduino host build, confirming it is a recognised way to
   satisfy this dependency without touching RealNetworks' code.

4. **`hlxclib/stdlib.h` added** (Skywave-authored, a bare
   `#include <stdlib.h>` passthrough): `buffers.c` and `sbr.c` include
   that path when neither `USE_DEFAULT_STDLIB` nor `ARDUINO` is defined
   yet at that point in the translation unit. Upstream never needs the
   file because it always builds with `ARDUINO` defined - confirmed by
   asking the GitHub API for `libhelix-aac/hlxclib` at the pinned commit
   and getting a 404. Same reasoning as (3): add a header rather than
   edit RPSL source. `Arduino.h` also gained an `#include <stdio.h>`,
   because `sbr.c` calls `printf()` in two OOM diagnostics without
   including it itself and real Arduino cores pull stdio in transitively.

No RPSL-licensed file other than `assembly.h` was edited. Nothing that
matters to decoding (bitstream parsing, Huffman tables, MDCT, SBR) was
touched.

## Configuration

- **SBR (spectral band replication) is ON.** This vendored copy's
  `aaccommon.h` already does:
  ```c
  #ifndef ESP8266
  #define AAC_ENABLE_SBR 1
  #endif
  ```
  i.e. SBR is compiled in for any build that isn't ESP8266 (RAM-
  constrained). Skywave defines neither `ESP8266` nor
  `HELIX_FEATURE_AUDIO_CODEC_AAC_SBR` - the latter is the "canonical"
  vanilla-Helix SBR switch (see `aacdec.h`), but defining it here as well
  would collide with the line above (`aacdec.h` would try to
  `#define AAC_ENABLE_SBR` a second time with a different token, which
  is a macro-redefinition warning that `-Werror` in `tests/Makefile`
  would turn into a build failure). So: SBR is enabled by relying on this
  fork's own default, and `HELIX_FEATURE_AUDIO_CODEC_AAC_SBR` is
  deliberately left undefined everywhere in this project.
- Helix has SBR but not Parametric Stereo. HE-AACv2 stations (SBR + PS)
  will decode correctly in mono rather than stereo. This is a known,
  accepted limitation - see the project task notes - not a bug in this
  vendoring.
- ARM/x86 inline assembly is **disabled** for this target (see "Local
  changes" above); every file here compiles as portable C only.
- `AAC_MAX_NCHANS` is left at its default of 2 (`aacdec.h`). Skywave only
  ever asks for mono or stereo output.

## What was NOT verified

It compiles. As of 2026-09-07 00:44 the whole directory builds clean
under devkitARM with zero errors and zero warnings as part of the normal
`tools/build.sh cia` run, producing a `skywave.cia`. That proves the
`__3DS__` branch of `assembly.h` is selected and that the shims satisfy
every include - nothing more.

What is still unverified:

- **No AAC stream has ever been decoded on a 3DS**, in an emulator or on
  hardware. The `.cia` has not been booted since this landed.
- **SBR/HE-AAC is entirely unexercised.** `tests/test_aac.c`'s fixture is
  plain AAC-LC (ffmpeg's native encoder does not produce SBR), so the
  SBR path enabled above has zero test coverage on any platform.
- Host testing (`tests/test_aac.c`, plain gcc) proves the decoder logic
  and the bridge's reset semantics, but not `-march=armv6k` codegen,
  fixed-point behaviour on the real VFPv2 core, or decode timing on the
  console's ARM11.
