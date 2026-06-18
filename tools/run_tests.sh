#!/usr/bin/env bash
# Regression harness: run all test ROMs and report a pass count.
# The autonomous loop's gate metric — this number must strictly increase.
#
# Three categories:
#   serial    : ROM prints "Passed"/"Failed" to the link port (Blargg-style).
#   image     : ROM renders a deterministic frame; pixel-diff vs an official PNG.
#   framehash : ROM renders a deterministic frame; sha256 vs a verified-once hash.
#
# Exit code = number of FAILED tests (0 = all green).
set -u
cd "$(dirname "$0")/.."

BIN=./gbemu
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 99; }

pass=0; fail=0; total=0
row() { printf "%-46s %s\n" "$1" "$2"; }
row "TEST" "RESULT"
printf -- '%.0s-' {1..62}; echo

# Screen-only ROMs handled by framehash (or skipped); keep them out of serial.
SCREEN_ONLY=("halt_bug.gb" "interrupt_time.gb")
is_screen() { local b; b=$(basename "$1"); for s in "${SCREEN_ONLY[@]}"; do [ "$b" = "$s" ] && return 0; done; return 1; }

# --- serial ROMs ---
while IFS= read -r rom; do
    is_screen "$rom" && continue
    total=$((total+1))
    res=$("$BIN" "$rom" 2>&1 | grep -oE "RESULT: [A-Z/]+" | head -1); res=${res#RESULT: }
    if [ "$res" = "PASS" ]; then pass=$((pass+1)); row "${rom#roms/}" "PASS"
    else fail=$((fail+1)); row "${rom#roms/}" "${res:-TIMEOUT}"; fi
done < <(find roms -name '*.gb' -not -path 'roms/acid2/*' | sort)

# --- image ROMs (rom:reference.png:frames) ---
for spec in "roms/acid2/dmg-acid2.gb:tests/refs/dmg-acid2-ref.png:30"; do
    IFS=: read -r rom ref frames <<< "$spec"; total=$((total+1)); name="${rom#roms/} (img)"
    if [ ! -f "$rom" ] || [ ! -f "$ref" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    tmp="/tmp/gbemu_$(basename "$rom").raw"
    "$BIN" "$rom" --frames "$frames" --raw "$tmp" >/dev/null 2>&1
    if python3 tools/imgcmp.py "$ref" "$tmp" >/dev/null 2>&1; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL"; fi
done

# --- framehash ROMs (rom:frames:sha256) verified visually once ---
for spec in "roms/halt_bug.gb:200:af839267dfcc94c90f576235f84ad5a49ca01bfe98ea983e5e1446a586590c52"; do
    IFS=: read -r rom frames want <<< "$spec"; total=$((total+1)); name="${rom#roms/} (hash)"
    if [ ! -f "$rom" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    tmp="/tmp/gbemu_$(basename "$rom").hraw"
    "$BIN" "$rom" --frames "$frames" --raw "$tmp" --cycles 60000000 >/dev/null 2>&1
    got=$(shasum -a 256 "$tmp" | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL ($got)"; fi
done

# --- skipped (reported, not counted): CGB-oriented / not DMG-applicable ---
row "interrupt_time.gb (skip: CGB-oriented)" "SKIP"

printf -- '%.0s-' {1..62}; echo
printf "PASS: %d/%d   FAIL: %d   (+1 skipped)\n" "$pass" "$total" "$fail"
exit "$fail"
