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
# words "ALL SUITES PASSED".
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
if [ "$rc" -eq 0 ]; then
    echo "ALL SUITES PASSED"
else
    echo "SUITES FAILED (exit $rc)"
fi
exit "$rc"
