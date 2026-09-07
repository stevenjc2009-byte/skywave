#!/usr/bin/env bash
# Builds and runs the host test suites.
#
# These cover the parts of Skywave that are pure logic - the Icecast metadata
# demuxer, the audio ring buffer, the station-directory JSON parser and the URL
# rewrite behind the https->http retry - which are deliberately written without
# any 3DS header so they can be proven on a PC in a second instead of on
# hardware.
#
# Every suite added to tests/Makefile must also be added here. test_url spent a
# release being green in isolation and never once run by this script, which is
# indistinguishable from not existing: watch the total check count, not the
# words "ALL SUITES PASSED". That drift is now caught automatically rather
# than relied on by memory - see the suite-list consistency gate just below,
# which refuses to build anything at all if this script's list and tests/
# Makefile's TESTS line disagree, in either direction.
#
# Run from anywhere:  bash tools/run_tests.sh [path/to/capture.bin] [path/to/real.json]
#
# Both extra arguments are optional real-world samples, neither of which is
# committed (one is someone else's broadcast, the other a snapshot of their
# database). If you pass them:
#   - the demuxer is additionally checked by walking the MPEG frame chain
#     through its output, which only stays unbroken if stripping is byte-exact;
#   - the parser is additionally run over genuine API output.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$here"

# --- suite-list consistency gate --------------------------------------------
# The set of suites this script builds and runs, kept in exact sync with
# tests/Makefile's TESTS line by hand - and checked against it by machine
# right here, before a single file is compiled. Either direction of drift
# (a suite in one list but not the other) aborts the whole run with a named
# diff instead of silently skipping a suite and still printing
# "ALL SUITES PASSED", which is what happened to test_url and to sbrgrid.
SCRIPT_SUITES=(icy ring directory url httpmsg aac sbrgrid mirrors robust)

makefile_suites=$(grep -E '^TESTS[[:space:]]*:=' tests/Makefile | sed 's/^TESTS[[:space:]]*:=//') || true
mk_sorted=$(printf '%s\n' $makefile_suites | sed '/^$/d' | sort -u)
sh_sorted=$(printf '%s\n' "${SCRIPT_SUITES[@]}" | sort -u)

if [ "$mk_sorted" != "$sh_sorted" ]; then
    echo "SUITE LIST MISMATCH: tests/Makefile's TESTS line and this script's" >&2
    echo "SCRIPT_SUITES disagree on which suites exist. A suite missing from" >&2
    echo "either side runs silently nowhere, which is indistinguishable from a" >&2
    echo "suite that does not exist." >&2
    echo >&2
    echo "diff (< only in tests/Makefile TESTS, > only in tools/run_tests.sh SCRIPT_SUITES):" >&2
    diff <(echo "$mk_sorted") <(echo "$sh_sorted") >&2 || true
    exit 1
fi
# -----------------------------------------------------------------------------

cap="${1:-tests/capture.bin}"
json="${2:-tests/real.json}"
cc="${HOSTCC:-gcc}"
flags="-std=c11 -O2 -g -Wall -Wextra -Werror"
out="tests/build"

mkdir -p "$out"

echo "== building =="
$cc $flags -o "$out/test_icy"       tests/test_icy.c       source/net/icy.c
$cc $flags -o "$out/test_ring"      tests/test_ring.c      source/audio/ring.c
$cc $flags -o "$out/test_directory" tests/test_directory.c source/net/directory_parse.c
$cc $flags -o "$out/test_url"       tests/test_url.c       source/net/url.c
$cc $flags -o "$out/test_httpmsg"   tests/test_httpmsg.c   source/net/httpmsg.c

# aac: Skywave's own aac_bridge.c and test_aac.c build under the same $flags
# (including -Werror) as everything above. They link against ~30 vendored
# Helix AAC decoder objects (RealNetworks, RPSL-licensed - see
# source/audio/aac/README.md) that this project does not own and only patches
# for narrow portability reasons, compiled separately with relaxed warnings:
# one vendored file (sbr.c) has a pre-existing, harmless printf
# format-specifier mismatch in an OOM-only diagnostic ('%d' against a
# sizeof() result) that would otherwise be a hard -Werror failure unrelated
# to anything this test suite is actually checking. See tests/Makefile's own
# aac target for the identical reasoning - this mirrors it so `make aac` and
# this script never disagree about how the same code is built.
aac_objs=()
for f in source/audio/aac/*.c; do
    o="$out/aacvendor_$(basename "${f%.c}").o"
    $cc -std=c11 -O2 -g -Wall -Isource/audio/aac -c "$f" -o "$o"
    aac_objs+=("$o")
done
$cc $flags -Isource/audio/aac -o "$out/test_aac" tests/test_aac.c source/audio/aac_bridge.c "${aac_objs[@]}"

# mirrors: the only suite that compiles source/net/directory.c. It includes
# http.h -> tcp.h -> mbedtls, so directory.c cannot be built against the real
# header on a PC; tests/stub/http.h shadows it, and because `#include "http.h"`
# searches the including file's own directory before any -I, directory.c has to
# be COPIED next to the stub for the stub to win. Both copies are remade here
# every run so neither can drift from the tree. Mirrors the `mirrors` target in
# tests/Makefile - keep the two in step.
mkdir -p "$out/stub"
cp tests/stub/http.h source/net/directory.c "$out/stub/"
# -Isource/net is for the copy's own `#include "directory.h"`, which no longer
# has that header beside it. It does NOT let the real http.h back in: a quoted
# include takes the first hit, and the including file's own directory - holding
# the stub - is searched before any -I.
$cc $flags -Isource/net -o "$out/test_mirrors" \
    tests/test_mirrors.c "$out/stub/directory.c" source/net/directory_parse.c

# robust: the hostile-input suite. Built with ASan/UBSan and -fno-sanitize-recover
# so a memory error aborts rather than printing and carrying on, and at -O1
# because -O2 costs frames in the reports. SAN can be set empty on a toolchain
# with no sanitizer runtime (MinGW-w64 has none) - the checks still all run,
# they just lose the memory-safety net.
san="${SAN--fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
$cc -std=c11 -O1 -g -Wall -Wextra -Werror -Wno-unused-result $san -o "$out/test_robust" \
    tests/test_robust.c source/net/directory_parse.c source/net/url.c \
    source/net/httpmsg.c source/net/icy.c source/store/favourites.c

# sbrgrid: guards the numEnv clamps in source/audio/aac/sbrside.c, where a 2-bit
# field off the wire used to drive numEnv to 8 against a MAX_NUM_ENV of 5 and
# overrun both a struct field and a stack array. Built here rather than up with
# the other suites because it wants $san, which is defined just above.
#
# UnpackSBRGrid is static, so tests/test_sbrgrid.c INCLUDES sbrside.c to reach
# it - which means (a) this one TU carries vendored source and so gets the same
# relaxed warnings the vendored objects get rather than $flags/-Werror, and
# (b) aacvendor_sbrside.o must be left out of the link or every symbol in it is
# defined twice. Sanitized for the same reason test_robust is: the bug wrote one
# byte past a stack array, and ASan is what sees that directly. Mirrors the
# `sbrgrid` target in tests/Makefile - keep the two in step.
sbrgrid_objs=()
for o in "${aac_objs[@]}"; do
    [ "$o" = "$out/aacvendor_sbrside.o" ] || sbrgrid_objs+=("$o")
done
$cc -std=c11 -O1 -g -Wall $san -Isource/audio/aac -o "$out/test_sbrgrid" \
    tests/test_sbrgrid.c "${sbrgrid_objs[@]}"
echo "ok"
echo

rc=0
"./$out/test_ring" || rc=$?
echo
"./$out/test_icy" "$cap" || rc=$?
echo
"./$out/test_directory" "$json" || rc=$?
echo
"./$out/test_url" || rc=$?
echo
"./$out/test_httpmsg" || rc=$?
echo
"./$out/test_aac" || rc=$?
echo
"./$out/test_sbrgrid" || rc=$?
echo
"./$out/test_mirrors" || rc=$?
echo
"./$out/test_robust" || rc=$?

echo
if [ "$rc" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "SUITES FAILED (exit $rc)"
fi
exit "$rc"
