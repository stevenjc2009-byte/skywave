#!/usr/bin/env bash
# Convenience wrapper: fetch the two optional real-world samples, then run the
# host suites. Kept separate from run_tests.sh so that the suites themselves
# never depend on the network.
set -uo pipefail

# Self-locating: tools/ is one level below the project root, so the script
# works from any working directory and moves with the project. (Was an absolute
# /mnt/c/Users/... path, which both leaked a username into a public repo and
# only worked on one machine.)
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1

UA="Skywave/1.0.0 (Nintendo 3DS)"

if [ ! -s tests/real.json ]; then
    echo "fetching a directory response..."
    curl -s -A "$UA" \
      "http://de1.api.radio-browser.info/json/stations/search?codec=MP3&hls=0&hidebroken=true&order=clickcount&reverse=true&limit=40" \
      -o tests/real.json || true
fi

if [ ! -s tests/capture.bin ]; then
    echo "fetching a stream capture..."
    curl -s -H "Icy-MetaData: 1" -A "$UA" --max-time 40 --max-filesize 400000 \
      http://media-ice.musicradio.com/ClassicFMMP3 -o tests/capture.bin || true
fi

ls -l tests/real.json tests/capture.bin 2>/dev/null || true
echo

bash tools/run_tests.sh tests/capture.bin tests/real.json
echo "EXIT=$?"
