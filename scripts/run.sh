#!/usr/bin/env bash
# Runs the recompiled game headless on BozoEngine. Extra args go to tta-headless (--frames N, --pad-script FILE, ...).
source "$(dirname "$0")/common.sh"
BIN="$GAME_BUILD/tta-headless$EXE"
[ -x "$BIN" ] || { echo "missing $BIN, run scripts/build_runtime.sh first"; exit 1; }
exec "$BIN" "$@"
