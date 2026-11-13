#!/usr/bin/env bash
# The bench lock shared by the native ports on this machine, through BozoEngine/tools/bench_lock.sh: a timed game run
# or benchmark only starts when the lock is free, and holds it meanwhile. Builds take the bench alone through
# scripts/bench_run.sh --alone.
#   scripts/bench_lock.sh wait                         # until the lock is free
#   scripts/bench_lock.sh status
# Takes and releases are announced in the c2c f2f lane when the local hook build/announce.sh exists (not committed:
# it holds the room connection).
source "$(dirname "$0")/common.sh"
set +euo pipefail # the lock tool's checks return false while it waits
source "$ROOT/BozoEngine/tools/bench_lock.sh"
[ -x "$ROOT/build/announce.sh" ] && export PS2X_BENCH_LOCK_ANNOUNCE="$ROOT/build/announce.sh"

case "${1:-status}" in
    wait) bench_lock_wait ;;
    status) content=$(tr -d '\r' < "$(bench_lock_file)" 2>/dev/null); [ -n "$(echo "$content" | tr -d '[:space:]')" ] && echo "$content" || echo "free" ;;
    *) echo "usage: $0 wait|status"; exit 1 ;;
esac
