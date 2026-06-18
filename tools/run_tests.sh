#!/usr/bin/env bash
# Regression harness: run all test ROMs and report a pass count.
# The autonomous loop's gate metric — this number must strictly increase.
#
# Two categories:
#   serial : ROM prints "Passed"/"Failed" to the link port (Blargg-style).
#   image  : ROM renders a deterministic frame; diff vs an official PNG.
#
# Exit code = number of FAILED tests (0 = all green).
set -u
cd "$(dirname "$0")/.."

BIN=./gbemu
make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 99; }

pass=0; fail=0; total=0
row() { printf "%-44s %s\n" "$1" "$2"; }
printf "%-44s %s\n" "TEST" "RESULT"
printf -- "------------------------------------------------------------\n"

# --- serial ROMs ---
while IFS= read -r rom; do
    total=$((total+1))
    res=$("$BIN" "$rom" 2>&1 | grep -oE "RESULT: [A-Z/]+" | head -1)
    res=${res#RESULT: }
    if [ "$res" = "PASS" ]; then pass=$((pass+1)); row "${rom#roms/}" "PASS"
    else fail=$((fail+1)); row "${rom#roms/}" "${res:-TIMEOUT}"; fi
done < <(find roms -name '*.gb' -not -path 'roms/acid2/*' | sort)

# --- image ROMs (rom : reference.png : frames) ---
IMG_TESTS=(
    "roms/acid2/dmg-acid2.gb:tests/refs/dmg-acid2-ref.png:30"
)
for spec in "${IMG_TESTS[@]}"; do
    IFS=: read -r rom ref frames <<< "$spec"
    total=$((total+1))
    name="${rom#roms/} (img)"
    if [ ! -f "$rom" ] || [ ! -f "$ref" ]; then
        fail=$((fail+1)); row "$name" "MISSING"; continue
    fi
    tmp="/tmp/gbemu_$(basename "$rom").raw"
    "$BIN" "$rom" --frames "$frames" --raw "$tmp" >/dev/null 2>&1
    if python3 tools/imgcmp.py "$ref" "$tmp" >/dev/null 2>&1; then
        pass=$((pass+1)); row "$name" "PASS"
    else
        fail=$((fail+1)); row "$name" "FAIL"
    fi
done

printf -- "------------------------------------------------------------\n"
printf "PASS: %d/%d   FAIL: %d\n" "$pass" "$total" "$fail"
exit "$fail"
