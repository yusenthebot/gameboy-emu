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
done < <(find roms -name '*.gb' -not -path 'roms/acid2/*' -not -path 'roms/mooneye/*' -not -path 'roms/dmg_sound/*' -not -path 'roms/games/*' -not -path 'roms/mem_timing-2/*'  -not -path 'roms/wilbertpol/*' -not -path 'roms/same-suite/*' -not -path 'roms/mbc3-tester/*' -not -path 'roms/gbmicrotest/*' -not -path 'roms/blargg/*' -not -path 'roms/cgb-acid2/*' -not -path 'roms/cgb/*' -not -path 'roms/gambatte/*' -not -path 'roms/gambatte-cgb/*' | sort)

# --- image ROMs (rom:reference.png:frames) ---
for spec in "roms/acid2/dmg-acid2.gb:tests/refs/dmg-acid2-ref.png:30"; do
    IFS=: read -r rom ref frames <<< "$spec"; total=$((total+1)); name="${rom#roms/} (img)"
    if [ ! -f "$rom" ] || [ ! -f "$ref" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    tmp="/tmp/gbemu_$(basename "$rom").raw"
    "$BIN" "$rom" --frames "$frames" --raw "$tmp" >/dev/null 2>&1
    if python3 tools/imgcmp.py "$ref" "$tmp" >/dev/null 2>&1; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL"; fi
done

# --- CGB color image: cgb-acid2 rendered in color, pixel-diffed vs the reference ---
total=$((total+1))
"$BIN" roms/cgb-acid2/cgb-acid2.gbc --frames 40 --rgb /tmp/gbemu_cgb.rgb --cycles 20000000 >/dev/null 2>&1
if python3 tools/cgbcmp.py roms/cgb-acid2/cgb-acid2-ref.png /tmp/gbemu_cgb.rgb >/dev/null 2>&1; then
    pass=$((pass+1)); row "cgb-acid2 (color image)" "PASS"
else fail=$((fail+1)); row "cgb-acid2 (color image)" "FAIL"; fi

# --- CGB boot register state: boot_regs-cgb run as CGB hardware (--cgb) ---
total=$((total+1))
if "$BIN" roms/cgb/boot_regs-cgb.gb --cgb --mooneye --cycles 20000000 2>&1 | grep -q "RESULT: PASS"; then
    pass=$((pass+1)); row "boot_regs-cgb (CGB boot state)" "PASS"
else fail=$((fail+1)); row "boot_regs-cgb (CGB boot state)" "FAIL"; fi

# --- MBC3/MBC30 banking: mbc3-tester pixel-diffed vs reference (needs the 4MB ROM; gitignored) ---
if [ -f roms/mbc3-tester/mbc3-tester.gb ]; then
    total=$((total+1))
    "$BIN" roms/mbc3-tester/mbc3-tester.gb --frames 220 --rgb /tmp/gbemu_mbc3.rgb --cycles 80000000 >/dev/null 2>&1
    if python3 tools/cgbcmp.py roms/mbc3-tester/mbc3-tester-ref.png /tmp/gbemu_mbc3.rgb >/dev/null 2>&1; then
        pass=$((pass+1)); row "mbc3-tester (MBC30 banking)" "PASS"
    else fail=$((fail+1)); row "mbc3-tester (MBC30 banking)" "FAIL"; fi
else
    row "mbc3-tester (4MB ROM absent — see README)" "SKIP"
fi

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

# --- battery .sav: cart RAM (+ MBC3 RTC) persists across a save/load round-trip ---
total=$((total+1))
if "$BIN" roms/mooneye/emulator-only/mbc1/ram_64kb.gb --sav-selftest 2>&1 | grep -q "RESULT: PASS"; then
    pass=$((pass+1)); row "battery .sav round-trip" "PASS"
else fail=$((fail+1)); row "battery .sav round-trip" "FAIL"; fi

# --- Gambatte test suite (DMG): hex-digit-result ROMs render their value as 8x8
#     tiles (gambatte_check.py decodes vs the filename); outaudio ROMs are checked
#     for audio activity over the final frame (--apu-activity). 15 frames each. ---
gpass=0; gtot=0
while IFS= read -r grom; do
    gtot=$((gtot+1)); total=$((total+1)); gb_name=$(basename "$grom")
    if [[ "$gb_name" == *outaudio* ]]; then
        want=$(echo "$gb_name" | grep -oE "outaudio[01]" | head -1 | grep -oE "[01]")
        got=$("$BIN" "$grom" --apu-activity --cycles 1500000 2>/dev/null | grep -oE "audio[01]" | grep -oE "[01]")
        [ "$want" = "$got" ] && ok=1 || ok=0
    else
        "$BIN" "$grom" --frames 15 --rgb /tmp/gemb.rgb --cycles 1500000 >/dev/null 2>&1
        python3 tools/gambatte_check.py "$gb_name" /tmp/gemb.rgb dmg >/dev/null 2>&1 && ok=1 || ok=0
    fi
    if [ "$ok" = 1 ]; then gpass=$((gpass+1)); pass=$((pass+1))
    else fail=$((fail+1)); row "gambatte ${grom#roms/gambatte/}" "FAIL"; fi
done < <(find roms/gambatte \( -name '*.gb' -o -name '*.gbc' \) 2>/dev/null | sort)
[ "$gtot" -gt 0 ] && row "gambatte DMG suite" "$gpass/$gtot"

# --- Gambatte CGB suite: same ROMs run as CGB hardware (--cgb); the result digits
#     are black/white so they mask to the font identically regardless of RGB formula. ---
cpass=0; ctot=0
while IFS= read -r crom; do
    ctot=$((ctot+1)); total=$((total+1)); cb_name=$(basename "$crom")
    if [[ "$cb_name" == *outaudio* ]]; then
        want=$(echo "$cb_name" | grep -oE "cgb04c_outaudio[01]" | head -1 | grep -oE "[01]$")
        got=$("$BIN" "$crom" --cgb --apu-activity 2>/dev/null | grep -oE "audio[01]" | grep -oE "[01]")
        [ "$want" = "$got" ] && cok=1 || cok=0
    else
        "$BIN" "$crom" --cgb --frames 15 --rgb /tmp/gembc.rgb --cycles 2500000 >/dev/null 2>&1
        python3 tools/gambatte_check.py "$cb_name" /tmp/gembc.rgb cgb >/dev/null 2>&1 && cok=1 || cok=0
    fi
    if [ "$cok" = 1 ]; then cpass=$((cpass+1)); pass=$((pass+1))
    else fail=$((fail+1)); row "gambatte-cgb ${crom#roms/gambatte-cgb/}" "FAIL"; fi
done < <(find roms/gambatte-cgb \( -name '*.gb' -o -name '*.gbc' \) 2>/dev/null | sort)
[ "$ctot" -gt 0 ] && row "gambatte CGB suite" "$cpass/$ctot"

# --- CGB WRAM banking (SVBK) + VRAM DMA (HDMA general + HBlank) behavior ---
total=$((total+1))
if "$BIN" roms/cpu_instrs.gb --wram-selftest 2>&1 | grep -q "RESULT: PASS"; then
    pass=$((pass+1)); row "CGB WRAM banking (SVBK)" "PASS"
else fail=$((fail+1)); row "CGB WRAM banking (SVBK)" "FAIL"; fi
total=$((total+1))
if "$BIN" roms/cpu_instrs.gb --hdma-selftest 2>&1 | grep -q "RESULT: PASS"; then
    pass=$((pass+1)); row "CGB VRAM DMA (HDMA)" "PASS"
else fail=$((fail+1)); row "CGB VRAM DMA (HDMA)" "FAIL"; fi
# --- pixel-FIFO BG renderer (T-cycle PPU spike): reproduces the BG formula ---
total=$((total+1))
if "$BIN" roms/cpu_instrs.gb --fifo-selftest 2>&1 | grep -q "RESULT: PASS"; then
    pass=$((pass+1)); row "pixel-FIFO BG renderer (spike)" "PASS"
else fail=$((fail+1)); row "pixel-FIFO BG renderer (spike)" "FAIL"; fi

# --- rewind: in-memory snapshot round-trip + rewind/replay must be bit-identical ---
total=$((total+1))
if "$BIN" roms/games/libbet/libbet.gb --rewind-selftest >/dev/null 2>&1; then
    pass=$((pass+1)); row "rewind snapshot round-trip + replay" "PASS"
else fail=$((fail+1)); row "rewind snapshot round-trip + replay" "FAIL"; fi

# --- debugger: a scripted session (disasm/break/cont/mem/step) must produce
#     the exact deterministic output (guards the disassembler + debugger) ---
total=$((total+1))
DBG_WANT="954bf306a4e93071f8cdba0f650a8e2f0f9a786fff0267236c99f1c05b7889a2"
DBG_GOT=$(printf 'break 0x0637\ncont\nd 0x0637 3\nmem 0xFF40 8\nstep 2\nq\n' \
          | "$BIN" roms/cpu_instrs.gb --debug 2>/dev/null | shasum -a 256 | cut -d' ' -f1)
if [ "$DBG_GOT" = "$DBG_WANT" ]; then pass=$((pass+1)); row "debugger scripted session" "PASS"
else fail=$((fail+1)); row "debugger scripted session" "FAIL ($DBG_GOT)"; fi

# --- APU audio synthesis: a fixed run must produce deterministic, non-silent PCM.
#     (verified once to be audible; this guards the synth against regression) ---
AUDIO=(
  "roms/games/libbet/libbet.gb|600|2c52f2ffe83bdb1df9d61d6c6bdd16d74fa88ff6b90d0a02a71643c233ba38e5"
)
for spec in "${AUDIO[@]}"; do
    IFS='|' read -r rom fr want <<< "$spec"
    total=$((total+1)); name="${rom#roms/} audio@${fr}f"
    if [ ! -f "$rom" ]; then fail=$((fail+1)); row "$name" "MISSING"; continue; fi
    tmp="/tmp/gbemu_audio_$(basename "$rom").pcm"
    "$BIN" "$rom" --frames "$fr" --audio-raw "$tmp" --cycles 120000000 >/dev/null 2>&1
    got=$(shasum -a 256 "$tmp" | cut -d' ' -f1)
    if [ "$got" = "$want" ]; then pass=$((pass+1)); row "$name" "PASS"
    else fail=$((fail+1)); row "$name" "FAIL ($got)"; fi
done

# --- interactive frontend: gbplay (SDL2) must drive the engine to identical frames.
#     Runs headless under the dummy video driver. Skipped if SDL2 is unavailable. ---
if make play >/dev/null 2>&1; then
    total=$((total+1))
    SDL_VIDEODRIVER=dummy ./gbplay roms/games/libbet/libbet.gb --frames 600 --png /tmp/gbemu_play.png >/dev/null 2>&1
    "$BIN" roms/games/libbet/libbet.gb --frames 600 --png /tmp/gbemu_playref.png --cycles 100000000 >/dev/null 2>&1
    if cmp -s /tmp/gbemu_play.png /tmp/gbemu_playref.png; then pass=$((pass+1)); row "gbplay frontend (frame-match)" "PASS"
    else fail=$((fail+1)); row "gbplay frontend (frame-match)" "FAIL"; fi
else
    row "gbplay frontend (SDL2 unavailable)" "SKIP"
fi

# --- Mooneye acceptance ROMs (LD B,B breakpoint + Fibonacci register signature) ---
mooneye_pass=0
while IFS= read -r rom; do
    total=$((total+1))
    res=$("$BIN" "$rom" --mooneye --cycles 40000000 2>&1 | grep -oE "RESULT: [A-Z]+" | head -1); res=${res#RESULT: }
    if [ "$res" = "PASS" ]; then pass=$((pass+1)); mooneye_pass=$((mooneye_pass+1))
    else fail=$((fail+1)); row "${rom#roms/}" "${res:-TIMEOUT}"; fi
done < <(find roms/mooneye roms/wilbertpol roms/same-suite -name '*.gb' | sort)
row "mooneye/* ($mooneye_pass passing, shown only on fail)" "OK"

# --- GBMicrotest (aappleby): result byte at FF82 (0x01 pass / 0xFF fail) ---
gbmicro_pass=0
while IFS= read -r rom; do
    total=$((total+1))
    res=$("$BIN" "$rom" --gbmicro 2>&1 | grep -oE "RESULT: [A-Z]+" | head -1); res=${res#RESULT: }
    if [ "$res" = "PASS" ]; then pass=$((pass+1)); gbmicro_pass=$((gbmicro_pass+1))
    else fail=$((fail+1)); row "${rom#roms/}" "${res:-FAIL}"; fi
done < <(find roms/gbmicrotest -name '*.gb' 2>/dev/null | sort)
row "gbmicrotest/* ($gbmicro_pass passing, shown only on fail)" "OK"

# --- Blargg screen-output variants (result on the BG tilemap; --blargg scans it) ---
blargg_pass=0
while IFS= read -r rom; do
    total=$((total+1))
    case "$rom" in *cgb_sound*) cg="--cgb";; *) cg="";; esac
    res=$("$BIN" "$rom" $cg --blargg --cycles 300000000 2>&1 | grep -oE "RESULT: [A-Z]+" | head -1); res=${res#RESULT: }
    if [ "$res" = "PASS" ]; then pass=$((pass+1)); blargg_pass=$((blargg_pass+1))
    else fail=$((fail+1)); row "${rom#roms/}" "${res:-FAIL}"; fi
done < <(find roms/blargg -name '*.gb' 2>/dev/null | sort)
row "blargg/* ($blargg_pass passing, shown only on fail)" "OK"

# --- skipped (reported, not counted): CGB-oriented / not DMG-applicable ---
row "interrupt_time.gb (skip: CGB-oriented)" "SKIP"

printf -- '%.0s-' {1..62}; echo
printf "PASS: %d/%d   FAIL: %d   (+1 skipped)\n" "$pass" "$total" "$fail"
exit "$fail"
