#!/usr/bin/env bash
# Recompiles SLUS_210.27 with bozo-recomp and builds tta-headless -> build/bozo/tta-headless
# Usage: scripts/build_runtime.sh [Release|RelWithDebInfo|Debug]
source "$(dirname "$0")/common.sh"
BUILD_TYPE="${1:-Release}"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh <iso> first"; exit 1; }
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$GAME_BUILD" --target tta-headless -j "$JOBS"
