#!/usr/bin/env bash
# Runs one command under the shared bench lock (BozoEngine/tools/bench_lock.sh): waits for it, takes it, runs, and
# releases it on any exit (end, error, Ctrl-C, kill), so a stopped run never leaves the lock behind. A game run takes
# one slot; builds and timed runs take the bench alone (--alone).
#   scripts/bench_run.sh [--alone] <minutes> "<purpose>" <command> [args...]
source "$(dirname "$0")/common.sh"
set +euo pipefail # the lock tool's checks return false while it waits
source "$ROOT/BozoEngine/tools/bench_lock.sh"
[ -x "$ROOT/build/announce.sh" ] && export PS2X_BENCH_LOCK_ANNOUNCE="$ROOT/build/announce.sh"
alone=""
[ "${1:-}" = "--alone" ] && { alone="alone"; shift; }
minutes="${1:?minutes}"; purpose="${2:?purpose}"; shift 2
[ $# -gt 0 ] || { echo "usage: $0 [--alone] <minutes> <purpose> <command> [args...]"; exit 1; }

bench_lock_take "TLOTR-TTA-Recomp" "$minutes" "$purpose" $alone
trap bench_lock_release EXIT
trap 'exit 130' INT TERM HUP
"$@"
