#!/usr/bin/env bash
# Builds Skywave. MUST be run from the devkitPro MSYS2 shell - Git Bash cannot
# see devkitARM and WSL cannot see the Windows toolchain.
#
#   bash tools/build.sh          .3dsx (and .smdh)
#   bash tools/build.sh cia      the installable .cia as well
set -uo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

target="${1:-all}"

echo "== $(arm-none-eabi-gcc --version | head -1)"
echo

make "$target"
rc=$?

echo
echo "EXIT=$rc"
ls -l skywave.3dsx skywave.smdh skywave.cia 2>/dev/null
exit $rc
