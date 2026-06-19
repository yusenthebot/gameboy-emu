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
done < <(find roms -name '*.gb' -not -path 'roms/acid2/*' -not -path 'roms/mooneye/*' -not -path 'roms/dmg_sound/*' -not -path 'roms/games/*' -not -path 'roms/mem_timing-2/*'  -not -path 'roms/wilbertpol/*' -not -path 'roms/same-suite/*' | sort)

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
# Blargg dmg_sound subtests print "Passed" on screen (no serial); gated by frame hash.
FRAMEHASH=(
  "roms/halt_bug.gb:200:af839267dfcc94c90f576235f84ad5a49ca01bfe98ea983e5e1446a586590c52"
  "roms/mem_timing-2/01-read_timing.gb:300:3760ee8b57c6c2ab056d4b7d031f2faff9776798952ec86792d42612709d5182"
  "roms/mem_timing-2/02-write_timing.gb:300:9188cba9c59c99056ae1d3c826647f0cd20fc06e42d419d2b7e58813c2ce787d"
  "roms/mem_timing-2/03-modify_timing.gb:300:55c3c37f0cb5f9c806294aef2d8ca6c884c9067f61c1213df4bfcad8c22ac157"
  "roms/dmg_sound/01-registers.gb:1300:889a65d03aafcb1eaf083cc5c427d52131df4ea5f94b18696e4e0ba22c761980"
  "roms/dmg_sound/02-len ctr.gb:1300:fe526b2e378dba4ce9b8b840badcc38cc97dc624400ab78274d51e25754e1b03"
  "roms/dmg_sound/03-trigger.gb:1300:d91b281e25a0dbfd4e99d65a3f85a02af6890f895a524fe8fa3ee1082a6fcac9"
  "roms/dmg_sound/06-overflow on trigger.gb:1300:ab0917bbc7bb37e7391ee4d8ca6f081973efb96ddbd9b8348d5386a2f965dd5c"
  "roms/dmg_sound/11-regs after power.gb:1300:be757e3af99787512f4a5c1701c75750781689f8c013cd9d3743b2f9ce76e585"
)
for spec in "${FRAMEHASH[@]}"; do
    want="${spec##*:}"; rest="${spec%:*}"; frames="${rest##*:}"; rom="${rest%:*}"
    total=$((total+1)); name="${rom#roms/} (hash)"
    if [ ! -f "$rom" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    tmp="/tmp/gbemu_$(basename "$rom").hraw"
    "$BIN" "$rom" --frames "$frames" --raw "$tmp" --cycles 250000000 >/dev/null 2>&1
    got=$(shasum -a 256 "$tmp" | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL ($got)"; fi
done

# --- game demos: real homebrew renders correctly (frame-hash, optional input) ---
# format: rom|frames|keys|sha256  (keys "" = none; "f:btn,..." = scripted input)
GAMES=(
  "roms/games/libbet/libbet.gb|600||35b72daceddf64b6105cdc35991008844919f72e9bfcdcea4ffb0950f984d3b1"
  "roms/games/libbet/libbet.gb|900|650:start,665:none|4db317ab88bc6d4c75668d7e39fda77277c5385093c867616a206b39a40e059b"
)
for spec in "${GAMES[@]}"; do
    IFS='|' read -r rom fr gkeys want <<< "$spec"
    total=$((total+1)); name="${rom#roms/games/}${gkeys:+ +input} (game)"
    if [ ! -f "$rom" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    tmp="/tmp/gbemu_game_$(basename "$rom")_$fr.raw"
    if [ -n "$gkeys" ]; then "$BIN" "$rom" --frames "$fr" --keys "$gkeys" --raw "$tmp" --cycles 120000000 >/dev/null 2>&1
    else "$BIN" "$rom" --frames "$fr" --raw "$tmp" --cycles 120000000 >/dev/null 2>&1; fi
    got=$(shasum -a 256 "$tmp" | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL ($got)"; fi
done

# --- save-state determinism: snapshot at frame S, resume to T, must equal a
#     straight run to T (verifies the full machine snapshot across subsystems) ---
# format: rom|save_frame|total_frame
SAVESTATE=(
  "roms/games/libbet/libbet.gb|100|220"
  "roms/dmg_sound/01-registers.gb|80|160"
  "roms/acid2/dmg-acid2.gb|20|40"
)
for spec in "${SAVESTATE[@]}"; do
    IFS='|' read -r rom sf tf <<< "$spec"
    total=$((total+1)); name="${rom#roms/} savestate@$sf->$tf"
    if [ ! -f "$rom" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    base="/tmp/gbemu_ss_$(basename "$rom")"
    "$BIN" "$rom" --frames "$tf" --raw "$base.straight" --cycles 200000000 >/dev/null 2>&1
    "$BIN" "$rom" --frames "$sf" --save-state "$base.gss" --cycles 200000000 >/dev/null 2>&1
    "$BIN" "$rom" --frames "$tf" --load-state "$base.gss" --raw "$base.resumed" --cycles 200000000 >/dev/null 2>&1
    if cmp -s "$base.straight" "$base.resumed"; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL (nondeterministic)"; fi
done

# --- Mooneye acceptance ROMs (LD B,B breakpoint + Fibonacci register signature) ---
mooneye_pass=0
while IFS= read -r rom; do
    total=$((total+1))
    res=$("$BIN" "$rom" --mooneye --cycles 40000000 2>&1 | grep -oE "RESULT: [A-Z]+" | head -1); res=${res#RESULT: }
    if [ "$res" = "PASS" ]; then pass=$((pass+1)); mooneye_pass=$((mooneye_pass+1))
    else fail=$((fail+1)); row "${rom#roms/}" "${res:-TIMEOUT}"; fi
done < <(find roms/mooneye roms/wilbertpol roms/same-suite -name '*.gb' | sort)
row "mooneye/* ($mooneye_pass passing, shown only on fail)" "OK"

# --- skipped (reported, not counted): CGB-oriented / not DMG-applicable ---
row "interrupt_time.gb (skip: CGB-oriented)" "SKIP"

printf -- '%.0s-' {1..62}; echo
printf "PASS: %d/%d   FAIL: %d   (+1 skipped)\n" "$pass" "$total" "$fail"
exit "$fail"
