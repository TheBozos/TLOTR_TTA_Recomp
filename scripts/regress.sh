#!/usr/bin/env bash
# Visual regression check: runs the window build on a fixed set of scenes from the saved states (build/states, local),
# captures fixed frames, and compares them with a local baseline (tools/tta_regress.py).
#   scripts/regress.sh            capture into build/regress/new and compare with build/regress/baseline
#   scripts/regress.sh --update   capture straight into build/regress/baseline
#   scripts/regress.sh [--update] <scene>...   only those scenes (battle1 outpost forest rohan helmsdeep boot pause idrial
#                                                moria osgiliath minastirith balrog widescreen)
# Runs are deterministic (TTA_DETERMINISTIC=1, the window in lockstep): the game holds at each capture frame until the window has captured
# it, so the same build gives the same pixels. Build first (ninja -C build/bozo tta).
# Python with PIL, run without MSYS2 on the PATH (common.sh puts it first; its DLLs break this PIL).
HOST_PATH="$(printf '%s' "$PATH" | tr ':' '\n' | grep -v msys64 | paste -sd ':')"
PYTHON="$(PATH="$HOST_PATH" command -v python3 || PATH="$HOST_PATH" command -v python)"
source "$(dirname "$0")/common.sh"

update=0
[ "${1:-}" = "--update" ] && { update=1; shift; }
STATES="$ROOT/build/states"
PADS="$ROOT/tools/pads"
FOREST="--load-state $STATES/forest_14900.state"
NONE="--pad-script $PADS/pad_none.txt" # scripted: the window hidden, no sound

# name | arguments | frames
SCENES=(
    "battle1|--load-state $STATES/battle1_5800.state --pad-script $PADS/pad_attack.txt|5900 6048 6300 6800"
    "outpost|--load-state $STATES/outpost_3900.state --pad-script $PADS/pad_outpost.txt|4500 5451"
    "forest|$FOREST $NONE|14940 14971"
    "rohan|$FOREST --warp Rohan@15000 --encounter 1172@16200 --pad-script $PADS/pad_warp_battle.txt|16590 16750"
    "helmsdeep|$FOREST --warp HelmsDeep@15000 --encounter 1126@16200 --pad-script $PADS/pad_warp_battle.txt|19800 20400"
    "boot|$NONE|1500 2000 2380"
    "pause|$FOREST --pad-script $PADS/pad_pause.txt|15050 15140 15260"
    "idrial|--load-state $STATES/forest_9094.state $NONE|9450 9750"
    "moria|$FOREST --warp e01c02@15000 --encounter 1000@16200 --pad-script $PADS/pad_warp_battle.txt|16700 17300"
    "osgiliath|$FOREST --warp Osgiliath@15000 --encounter 1269@16200 --pad-script $PADS/pad_warp_battle.txt|16700 17900"
    "minastirith|$FOREST --warp e03c02@15000 --pad-script $PADS/pad_warp_battle.txt|16080 16200"
    "balrog|$FOREST --warp e01c04@15000 --pad-script $PADS/pad_warp_battle.txt|17250"
    "widescreen|--widescreen --load-state $STATES/battle1_5800.state --pad-script $PADS/pad_attack.txt|5900 6300"
)

out="$ROOT/build/regress/$([ $update = 1 ] && echo baseline || echo new)"
for entry in "${SCENES[@]}"; do
    IFS='|' read -r name args shots <<< "$entry"
    if [ $# -gt 0 ] && [[ " $* " != *" $name "* ]]; then
        continue
    fi
    dir="$out/$name"
    rm -rf "${dir:?}"
    mkdir -p "$dir"
    last=0
    capture=()
    for f in $shots; do
        capture+=(--capture-frame "$f")
        [ "$f" -gt "$last" ] && last=$f
    done
    echo "== $name"
    # shellcheck disable=SC2086 # the arguments are words on purpose
    TTA_DETERMINISTIC=1 "$ROOT/scripts/bench_run.sh" 30 "tta regress $name" "$GAME_BUILD/tta$EXE" $args \
        --frames $((last + 30)) --capture-dir "$dir" "${capture[@]}" > "$dir.log" 2>&1 || {
        echo "$name: the run failed, see $dir.log"
    }
done

if [ $update = 1 ]; then
    echo "baseline updated in $out"
    exit 0
fi
PATH="$HOST_PATH" "$PYTHON" "$ROOT/tools/tta_regress.py" "$ROOT/build/regress/baseline" "$out" "$@"
