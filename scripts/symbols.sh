#!/usr/bin/env bash
# Rebuilds recomp/tta_symbols.txt (SLUS_210.27 has no symbols) with BozoEngine's matcher, from reference executables
# that keep theirs and the names read by hand in recomp/tta_symbols_reviewed.txt. Report: build/bozo/tta_symbols_report.txt
# Usage: scripts/symbols.sh <reference.elf>...   (TTA's map used Dark Cloud's SCUS_971.11 and Twisted Metal: Black's
#        SCUS_971.01: the player's own discs, never committed)
source "$(dirname "$0")/common.sh"
[ $# -gt 0 ] || { echo "usage: $0 <reference.elf>..."; exit 1; }
mkdir -p "$ROOT/build/bozo"
refs=()
for ref in "$@"; do refs+=(--ref "$ref"); done
python -I "$ROOT/BozoEngine/tools/symbolmap/bozo_symbolmap.py" engine-names "$ROOT/BozoEngine" "$ROOT/build/bozo/want.txt"
python -I "$ROOT/BozoEngine/tools/symbolmap/bozo_symbolmap.py" match "$ELF" "${refs[@]}" \
    --reviewed "$ROOT/recomp/tta_symbols_reviewed.txt" --want "$ROOT/build/bozo/want.txt" \
    --out "$ROOT/recomp/tta_symbols.txt" --report "$ROOT/build/bozo/tta_symbols_report.txt"
head -2 "$ROOT/build/bozo/tta_symbols_report.txt"
