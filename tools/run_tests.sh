#!/usr/bin/env bash
# Regression harness: run every test ROM and report a pass count.
# The autonomous loop's gate metric — this number must strictly increase.
#
# Usage: tools/run_tests.sh [roms_glob]
# Exit code is the number of FAILED/TIMEOUT roms (0 = all green).
set -u
cd "$(dirname "$0")/.."

BIN=./gbemu
if [ ! -x "$BIN" ]; then
    echo "building..."; make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 99; }
fi

# Serial-based ROMs: pass/fail is read from the "Passed"/"Failed" marker.
# Add new categories (acid2 PNG diff, frame-hash) as later rounds land them.
mapfile -t ROMS < <(find roms -name '*.gb' | sort)

pass=0; fail=0; total=0
printf "%-44s %s\n" "ROM" "RESULT"
printf -- "------------------------------------------------------------\n"
for rom in "${ROMS[@]}"; do
    total=$((total+1))
    res=$("$BIN" "$rom" 2>&1 | grep -oE "RESULT: [A-Z/]+" | head -1)
    res=${res#RESULT: }
    case "$res" in
        PASS) pass=$((pass+1)); tag="PASS" ;;
        FAIL) fail=$((fail+1)); tag="FAIL" ;;
        *)    fail=$((fail+1)); tag="${res:-TIMEOUT}" ;;
    esac
    printf "%-44s %s\n" "${rom#roms/}" "$tag"
done
printf -- "------------------------------------------------------------\n"
printf "PASS: %d/%d   FAIL: %d\n" "$pass" "$total" "$fail"
exit "$fail"
