#!/usr/bin/env bash
# Builds a release CIA named the way the in-app updater expects to find it.
#
# The updater downloads
#   https://github.com/<owner>/skywave/releases/download/v<ver>/skywave<ver>.cia
# so the asset name is not cosmetic - a release whose file is called anything
# else installs by hand and fails from inside the app, which is the confusing
# way round. This script takes the version out of source/version.h rather than
# an argument, so the binary and the filename can never disagree.
#
# MUST be run from the devkitPro MSYS2 shell.
#
#   bash tools/release.sh
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

ver=$(sed -n 's/.*SKYWAVE_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' source/version.h)
if [ -z "$ver" ]; then
    echo "could not read SKYWAVE_VERSION out of source/version.h" >&2
    exit 1
fi

echo "version $ver"

# A clean build, always. The Makefile tracks header dependencies, but a release
# is the one build where "probably up to date" is not good enough.
make clean >/dev/null
make cia || exit 1

out="skywave${ver}.cia"
cp -f skywave.cia "$out" || exit 1

echo
echo "release asset: $out"
ls -l "$out"
md5sum "$out"

cat <<EOF

Next, by hand:
  1. git tag v$ver && git push origin v$ver
  2. Create the GitHub release for tag v$ver
  3. Upload $out as an asset, named exactly that
  4. Verify: the updater follows
     https://github.com/<owner>/skywave/releases/latest
     to /releases/tag/v$ver, then downloads the asset above.
EOF
