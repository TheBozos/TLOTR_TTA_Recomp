#!/usr/bin/env bash
# Recompiles SLUS_210.27 with bozo-recomp and builds tta-headless and the window build tta -> build/bozo/
# (TTA_WINDOW=OFF: tta-headless only)
# Usage: scripts/build_runtime.sh [Release|RelWithDebInfo|Debug]
source "$(dirname "$0")/common.sh"
BUILD_TYPE="${1:-Release}"
[ -f "$ELF" ] || { echo "missing $ELF, run scripts/extract_iso.sh <iso> first"; exit 1; }
cmake -S "$ROOT" -B "$GAME_BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" -DTTA_WINDOW="${TTA_WINDOW:-ON}"
targets=(tta-headless); [ "${TTA_WINDOW:-ON}" = ON ] && targets+=(tta)
cmake --build "$GAME_BUILD" --target "${targets[@]}" -j "$JOBS"
