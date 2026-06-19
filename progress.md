# Progress Log

## Round 17 — pixel-FIFO OBJ mode-3 penalty (PASS 118->119, FRONTIER CRACKED)  [committed + pushed]

### What was built (the owner-chosen frontier: FIFO sprite penalty)
- Got the exact algorithm via `gh api repos/gbdev/pandocs/contents/src/Rendering.md` (WebFetch
  was 403/404; gh api bypassed it). Pan Docs "OBJ penalty algorithm", implemented in
  obj_mode3_penalty() (ppu.c), cached per line at the mode-2->3 transition, fed into mode3_end:
  - objects sorted by (x asc, OAM index); X>=168 (off-screen right) skipped.
  - per object: if its BG tile (bg_pos = x-8+SCX, >>3) is not yet considered, add
    max(0, (7 - (bg_pos&7)) - 2); X=0 forces offset 0. Then a flat +6.
- KEY calibration: my scanline model detects mode 0 ~3 dots late vs a per-dot fetcher, so the
  per-line penalty is reduced by 3 dots. Found by forcing a fixed penalty (testcase 1, X=0,
  expects extra 2, passes for 6-8 dots not Pan Docs' 11) then sweeping the offset: offset 3
  passes ALL 105 testcases. Verified the algorithm offline first: replicated it in Python over
  the oracle -> expected_extra == floor(my_dots/4), monotonic, zero contradictions (after the
  two fixes: X=0->offset0, X>=168 excluded).
- +intr_2_mode0_timing_sprites. No regression (acid2 0/23040, libbet title+gameplay unchanged,
  intr_2_mode0/mode3/oam_ok still pass). Also flips the wilbertpol gpu name-dup (not gated).

### What did NOT work / notes
- The Pan Docs X=0 "always 11 dots" wording = offset-0 behavior (5+6), NOT +11 per object;
  treating it as flat-11-each made 10-at-X=0 give 110 dots vs X=8's 65 (both expect extra 16).
- Forgot X>=168 exclusion first -> off-screen-right objects wrongly penalized (extra 0 cases).

## Round 16 — VRAM blocking + sprite-FIFO groundwork (PASS 118->118, FLAT)  [committed local]

### What was attempted (the named frontier: FIFO sprite penalty)
- Target: Mooneye intr_2_mode0_timing_sprites (highest-leverage PPU test). Extracted the full
  105-case oracle. Derived the single-column table: extra(n,c)=floor(3n/2)+bonus, bonus=1 iff
  c<T(n) with T N-DEPENDENT (T(1)=4, T(10)=2), plus a cross-group cost. A closed form fits the
  first cases but not all -> genuine pixel-FIFO behaviour needing a dot-stepped fetcher sim.
- Insight: the test's "extra" is in M-cycles (~60 dots/10 objs ~= 16), so the sim must compute
  mode-3 in DOTS and let the existing calibrated mode-0 poll convert. Saved everything to
  docs/ppu-mode3-sprite-penalty.md.
- WHAT DID NOT WORK: the floor(3n/2)+c closed form (reverted, won't ship wrong timing).
  WebFetch of Pan Docs / GBEDG OBJ-penalty algorithm 403/404 (travel network).

### What shipped (verified)
- VRAM access blocking: CPU reads of 0x8000-0x9FFF return 0xFF and writes are ignored during
  mode 3 (ppu_vram_accessible = reported mode != 3). Sibling of round-13 OAM blocking. Verified
  no regression (acid2 0/23040, libbet unchanged, gate 118). Unlocks 0 currently-gated tests
  (correctness only) -> the gate stayed FLAT this round.

### Honest note
- First flat-gate round. Free/medium wins exhausted; remaining tail is all multi-round. Surfaced
  a decision point to the owner (STATUS round-17 seed): build the pixel-FIFO sim (A), APU wave
  channel (B), or pivot to the interactive playable frontend (C). Not pushed (flat round).

## Round 1 — SM83 CPU core (FLOOR met + exceeded)  [committed]

### What was built
- `src/gb.h` — types + single `GB` state struct (registers, memory, subsystem state).
- `src/cpu.c` — SM83 core, instruction-stepped. All 256 base + 256 CB opcodes,
  exact flag semantics (DAA, ADD HL half-carry, ADD SP/LD HL,SP+e nibble/byte carry),
  interrupt dispatch (5 vectors, 20-cycle service), EI 1-instr delay, DI, RETI,
  HALT + HALT-bug (PC no-increment double fetch).
- `src/bus.c` — full memory map + I/O dispatch + OAM DMA (instant) + joypad read.
- `src/cart.c` — ROM-only + MBC1 (banking, RAM enable, mode). cpu_instrs is MBC1.
- `src/timer.c` — 16-bit DIV, falling-edge TIMA increment, 4-cycle overflow reload delay.
- `src/serial.c` — capture SB on SC=0x81 write, emit to log+stdout, raise serial IRQ.
- `src/ppu_lite.c` — free-running LY/STAT/VBlank, NO rendering (prevents vblank-wait hangs).
- `src/main.c` — headless harness: run to "Passed"/"Failed", exit code.
- `tools/run_tests.sh` — regression harness, prints PASS count (the loop gate).

### Verified (real runs, not on paper)
- cpu_instrs.gb (combined): `01:ok ... 11:ok  Passed`, exit 0, ~224M cycles, 0.57s.
- instr_timing.gb: Passed (per-instruction cycle counts are exact — frontier freebie).
- Individuals: 8/8 valid sub-tests pass (07,08 re-fetched; 05 mirror serves a broken
  832B blob — combined run covers test 05 = ok).
- `./tools/run_tests.sh` => PASS: 12/12, exit 0.

### What did NOT work / notes
- Rust/rustup install: static.rust-lang.org timed out mid-download (flaky travel net).
  Chose C+clang (zero-net, canonical for cycle-accurate emus). Reconsider Rust migration
  only if net stabilizes AND a real architectural reason appears (spike-then-migrate).
- ROM mirror retrio/gb-test-roms serves a truncated `05-op rp.gb` (832B) repeatedly.
  Not an emulator bug. Combined ROM covers it.
- DESIGN DEBT (intentional, the long tail): subsystems are ticked once per instruction,
  NOT per M-cycle. This passes cpu_instrs + instr_timing but will FAIL mem_timing,
  mem_timing-2, and most Mooneye sub-instruction timing tests. Migrating the bus to
  per-M-cycle ticking is a core frontier item once the PPU exists.

## Round 2 — scanline PPU + dmg-acid2 gate (PASS 12->13)  [committed]

### What was built
- `src/ppu.c` (replaces ppu_lite) — full scanline renderer: BG, window (own line
  counter), sprites with X/Y flip, 8x16, OBP0/1, BG-vs-OBJ priority, lower-X+OAM-index
  draw priority, 10-sprites/line limit. Mode state machine (2/3/0/1), STAT + VBlank IRQ.
  Renders each line at the mode3->0 edge into `gb->fb` (160x144 shade indices 0..3).
- `src/png.c` — dependency-free 8-bit grayscale PNG writer (uncompressed zlib "stored"
  blocks + hand-rolled crc32/adler32). For visual frame dumps you can `open`.
- `src/main.c` — added frame-dump mode: `--frames N [--png p][--raw p][--cycles C]`.
- `tools/imgcmp.py` — pure-Python PNG decoder (handles 2-bit grayscale + all filters) that
  diffs the official reference against the emulator raw dump. Writes a diff map PNG too.
- `tools/run_tests.sh` — now runs serial ROMs AND image ROMs (acid2). Gate = 13/13.

### Verified (real runs)
- dmg-acid2: **0 / 23040 pixel mismatches** vs official reference-dmg.png, first run.
  Frame shade histogram {0:12849, 1:6254, 2:188, 3:3749} — a real image, not degenerate.
- cpu_instrs frame dump renders the on-screen "01:ok 02:ok..." text correctly.
- Regression: `./tools/run_tests.sh` => PASS: 13/13, exit 0 (no CPU regression).

### What did NOT work / notes
- No free Tetris ROM (copyrighted) — used dmg-acid2 as the deterministic visual gate
  instead (harder + has an official reference). A free homebrew title-screen + frame-hash
  check can be added later if a ROM is fetched.
- Rendering samples registers once per line (at mode3->0). Fine for acid2 (no mid-line
  register tricks). Mid-scanline SCX/SCY/palette changes and variable mode-3 length are
  part of the sub-instruction timing frontier.
- DESIGN DEBT still open: subsystems ticked once per instruction (not per M-cycle).
  Blocks mem_timing / Mooneye sub-instruction tests. This is round 3's main target.

## Round 3 — per-M-cycle cycle-accurate timing (PASS 13->17)  [committed]

### What was built
- Rewrote `src/cpu.c` timing model: a single `tick(g,t)` advances timer/PPU/serial; every
  memory access (`rd`/`wr`/`imm8`/`imm16`/`push16`/`pop16`) ticks 4T, and every internal
  M-cycle (INC/DEC rr, ADD HL, PUSH/RST/RET/CALL internal, conditional-taken penalty,
  ADD SP, LD HL/SP, interrupt dispatch) ticks explicitly. **tick-before-access** (access on
  the M-cycle's last T). cpu_step no longer end-ticks; cycle budget emerges from ticks.
- Interrupt service ticks 2+push(2)+1 = 5 M-cycles (20T).
- Harness: added a frame-hash category (sha256 of a deterministic frame, verified once).
  Excludes screen-only ROMs from the serial sweep; reports interrupt_time as SKIP.

### Verified (real runs)
- NO regression: cpu_instrs 11/11, instr_timing, acid2 0/23040 all still green.
- NEW PASS: mem_timing 01-read / 02-write / 03-modify (serial) — sub-instruction memory
  timing now correct (was all FAIL under instruction-stepped). tick-before-access was the
  right polarity on the first try.
- halt_bug: screen shows "Passed" (HALT-bug correct); gated via frame-hash (af839267...,
  deterministic across frames 150/200/250 and repeat runs).
- `./tools/run_tests.sh` => PASS: 17/17, exit 0.

### What did NOT work / notes
- interrupt_time: screen shows "Failed" — it's CGB-oriented (interrupt timing differs
  DMG vs CGB double-speed). Deferred to a CGB round, marked SKIP (not chased on DMG).
- Combined mem_timing.gb + mem_timing-2.gb: retrio mirror keeps truncating the larger
  files; used the 3 individual mem_timing ROMs instead (cleaner, 3 gate entries).
- Quirks NOT yet modeled (next targets): interrupt dispatch cancel/IE-overwrite quirk,
  fine timer write-during-reload edge cases, OAM DMA timing, variable mode-3 length.

## Round 4 — Mooneye integration + cycle-accurate OAM DMA (PASS 17->63)  [committed]

### What was built
- `--mooneye` harness mode (main.c): run to the LD B,B (0x40) software breakpoint, then
  check the Fibonacci register signature (B3 C5 D8 E13 H21 L34 = pass). Mooneye's protocol.
- Cycle-accurate OAM DMA (src/bus.c `dma_tick` + state in gb.h): 160 M-cycles, one byte/
  M-cycle, startup delay = 3 M-cycles (calibrated to oam_dma_timing), OAM locked to the CPU
  (reads $FF, writes ignored) while running, restart on re-write of FF46. Wired into cpu.c tick().
- run_tests.sh: added a mooneye category (vendored passers under roms/mooneye/).
- Vendored 46 passing DMG Mooneye acceptance ROMs + ATTRIBUTION (MIT, Gekkio).

### The key insight (why OAM DMA mattered so much)
- Diagnosed the instruction-timing cluster (push/call/jp/ret/rst/reti/add_sp/ld_hl_sp +
  cc variants) by fetching push_timing.s: Mooneye uses OAM DMA as the timing PROBE — it
  starts a DMA (locking OAM), does the instruction writing into OAM, aligned so the DMA
  ends mid-instruction, then reads OAM back to see exactly which M-cycle each access hit.
  Instant DMA -> OAM never locks -> the whole cluster fails. One feature flipped ~15 tests.
- oam_dma_timing.s gave the exact calibration: read at write+161 must see $FF (running),
  write+162 must see $01 (done) -> startup delay must be 3 (delay=2 ended one cycle early).

### Verified (real runs)
- No regression: cpu_instrs, instr_timing, mem_timing, acid2 (0/23040), halt_bug all green.
- Mooneye DMG acceptance: 31 -> 46 passing (OAM DMA added the instruction-timing cluster).
- `./tools/run_tests.sh` => PASS: 63/63, exit 0.

### What did NOT work / notes
- Network: the 3.76MB test-roms zip would not download in one shot (flaky travel link kept
  timing out ~1.4MB). Solved with a resilient 100KB-chunk range-request downloader that
  refetches only missing chunks until zip integrity passes (4 passes). Reusable pattern.
- 20 Mooneye tests still fail (frontier; see STATUS): ppu/* mode-timing (8), timer write-
  reload quirks (3), oam_dma_start + sources, unused_hwio, ie_push, rapid_di_ei, boot_*.

## Round 5 — timer write-reload quirks + HWIO read masks (PASS 63->66)  [committed]

### What was built
- timer.c: shared `timer_signal`/`tima_step` helpers; TAC-write and DIV-write now detect a
  falling edge on (selected DIV bit & enable) -> glitch TIMA increment. Added `tima_reloaded`
  flag (set on the reload cycle, cleared each M-cycle tick) so FF05/FF06 writes can honor:
  TIMA write on the reload cycle is ignored (reload wins); TMA write on the reload cycle
  makes the reload use the new TMA. (Writes during the delay still cancel the reload.)
- bus.c: HWIO read-OR mask table (HWIO_OR[0x80]) — unmapped I/O reads 0xFF, sound regs force
  their unused bits to 1. Makefile: -Wno-initializer-overrides for the range-designator table.

### Verified (real runs)
- New PASS: timer/tima_write_reloading, timer/tma_write_reloading, bits/unused_hwio-GS.
- No regression: cpu_instrs, mem_timing, instr_timing, acid2 (0/23040), all prior mooneye.
- `./tools/run_tests.sh` => PASS: 66/66, exit 0. Mooneye DMG acceptance 46 -> 49.

### What did NOT work / deferred (with reasons)
- timer/rapid_toggle: TAC-disable glitch is implemented but the test asserts an exact bc
  ($FFD9) => the cumulative glitch count must be cycle-perfect; mine is off by ~1 near a
  div-bit-9 boundary. Needs finer instrumentation. Deferred.
- interrupts/ie_push: the IE-overwrite cancel quirk has subtle multi-round semantics (which
  PC byte hits $FFFF, when IE is sampled to pick the vector vs cancel to PC=$0000). Risk of
  regressing passing interrupt tests; deferred to a focused study.
- oam_dma_start / oam_dma/sources-GS: precise DMA lock-start cycle (probed by executing code
  from OAM as DMA overwrites it) + CPU-read-returns-DMA-byte bus conflict. Needs a fuller
  DMA-conflict model. Deferred.

## Round 15 — completeness sweep (PASS 115->118)  [committed]

### What was done
- The easy/medium feature wins are exhausted, so ran a completeness critic: swept EVERY DMG
  test ROM across mooneye + same-suite + wilbertpol with --mooneye, listing passers whose
  name isn't already gated. Found +3 free passers (no emulator code needed):
  - same-suite/apu/channel_3/channel_3_wave_ram_dac_on_rw (first SameBoy-suite test gated)
  - same-suite/apu/div_write_trigger_10
  - wilbertpol/emulator-only/mbc1_rom_4banks
- Vendored them + new roms/same-suite category (SameSuite, LIJI32, MIT). Wired same-suite into
  the mooneye sweep; excluded it from the serial sweep (the recurring double-count trap).
- Attempted boot_div via a div_counter sweep (0xAB00..0xABFF) — all FAIL: it needs boot-handoff
  timing modeling, not just the DIV value. Reverted the hack.

### Verified
- No regression; gate 115 -> 118. No new emulator code (pure coverage discovery).

## Round 14 — boot/power-on state (PASS 113->115)  [committed]

### What was built
- Post-boot HWIO fixes (found by rendering boot_hwio's "MISMATCH AT $FFxx EXPECTED/GOT" screen
  and chasing the chain): JOYP io[0x00]=0x00 (reads 0xCF, was 0xFF); APU ch_on[0]=true so
  channel 1 is active post-boot and NR52 reads 0xF1 (was 0xF0).
- +boot_hwio-dmgABCmgb (Mooneye) and +boot_regs-dmg (Wilbert Pol — the only non-duplicate
  passing WP test left to vendor).

### Verified
- No regression: full gate 113->115, libbet title+gameplay hashes unchanged (games write the
  JOYP select bits before reading), dmg_sound 01/11 hashes unchanged (boot state vs power-cycle
  state are independent). boot_div still fails (post-boot DIV-at-handoff timing — deferred).

## Round 13 — OAM access blocking (PASS 112->113)  [committed]

### What was built
- CPU access to OAM (0xFE00-0xFE9F) is now blocked during PPU modes 2/3: reads return 0xFF,
  writes are ignored. Uses ppu_oam_accessible() = (reported mode <= 1), so the access window
  matches the +8 STAT-field timing the intr_2 tests measure. The PPU's own OAM reads and OAM
  DMA writes go direct (unaffected).
- +intr_2_oam_ok_timing (it polls cleared OAM until readable = mode 0). PPU cluster 6->7/12.

### Verified
- No regression: acid2 0/23040, libbet frame-hash unchanged (games access OAM via DMA/vblank),
  full gate 112->113. (Also flips wilbertpol gpu intr_2_oam_ok, a name-dup, not separately gated.)

### Note: chose intr_2_oam_ok (contained) over the sprite mode-3 penalty (intr_2_mode0_timing_
  sprites) — the latter is genuine FIFO-fetcher behavior (penalty depends on sprite count AND
  X alignment non-linearly; exact data in the .s) and wants a fetcher-timing simulation.

## Round 12 — Wilbert Pol suite (0xED breakpoint) (PASS 106->112)  [committed]

### What was found / built
- The user-named "Wilbert Pol" suite (mooneye-test-suite-wilbertpol, 121 ROMs) baselined at
  0/102 — but the reg dumps showed the Fibonacci PASS signature. Root cause: wilbertpol uses
  the illegal opcode 0xED as its completion breakpoint, not Mooneye's LD B,B (0x40). Traced
  the loop bytes (set fib regs, `ED`, `jr -3`) to confirm.
- main.c --mooneye now accepts 0x40 OR 0xED as the breakpoint. Safe: real code never executes
  illegal opcodes, and it only applies in --mooneye mode. No regression (standard mooneye
  still 106; LD B,B still detected).
- Result: wilbertpol DMG 0/102 -> 54/102. Vendored the 6 genuinely-NEW
  intr_2_mode0_scx{1,2,3,5,6,7}_timing_nops (validate the round-8 SCX mode-3 penalty across
  scx=1..7) under roms/wilbertpol/. Gate 106 -> 112.

### Frontier note
- wilbertpol acceptance/gpu/ = 11/47; the ~36 fails are the same PPU frontier (sprite/OAM
  mode-3 penalties, lcdon, window timing). A sprite/OAM mode-3 penalty would unlock many at once.

## Round 11 — mem_timing-2 + PPU LCD/STAT quirks (PASS 102->106)  [committed]

### What was built
- mem_timing-2 (Blargg, screen-output; I already pass mem_timing): verified "Passed"
  visually, frame-hashed the 3 individuals (01-read/02-write/03-modify). +3.
- PPU LCD/STAT quirks (src/ppu.c) -> Mooneye stat_lyc_onoff (+1):
  - LCD off: the LY=LYC coincidence flag FREEZES at its turn-off value (g->ly_coin), and
    the STAT mode field reads 0.
  - First frame after LCD-on: LY=0's OAM-scan period reports mode 0 (g->lcd_on_frame).
  - The STAT interrupt line is now GB state (g->stat_line), set at LCD-off to the FROZEN
    coincidence value so a later LCD-on only re-fires the LYC IRQ on a genuine rising edge
    (round 2 frozen-1 = no edge; round 4 frozen-0 = edge). LCD-on evaluates it immediately
    so the IRQ lands within the LCDC-write instruction.

### How the round-4 bug was found (systematic)
- Traced STAT reads (rounds 1-3 fixed by the freeze + first-frame mode-0), then traced the
  interrupt service: the IRQ *was* requested but round 2 also fired it (wrong). Root cause:
  resetting stat_line to false on LCD-off lost the frozen-edge state. Setting it to the
  frozen coincidence fixed both.

### Verified
- No regression (acid2 0/23040, all prior STAT tests still pass). Gate 102 -> 106.

## Round 10 — playability: a real game runs (PASS 100->102)  [committed]

### What was built / proven
- src/main.c: `--keys "frame:btn,..."` scripted joypad input (right/left/up/down/a/b/
  select/start/none, held until the next event) for headless playability testing.
- Vendored libbet ("Libbet and the Magic Floor", Damian Yerrick / PinoBatch, zlib) under
  roms/games/libbet/. New "game" gate category (rom|frames|keys|sha256).
- Proven on a REAL game: libbet renders its **playable title screen** ("Select: demo |
  Start: play"), responds to a scripted Start press, and renders **actual gameplay** (game
  board + HUD "0 Combo / 0% / 0/04"). Both frames are static/deterministic and frame-hashed.
- Also ran uCity (AntonioND, MBC5, 128KB, CGB-only): correctly renders its "This game is
  only for GBC!" DMG-detection screen — proves MBC5 + accurate DMG register behavior.

### How the ROM was obtained (flaky network)
- Tobu Tobu Girl / 2048-gb fetches returned HTML (bad paths); uCity is CGB-only. libbet has
  a direct GitHub release asset (validated via the Nintendo-logo header check at 0x104).

### Verified
- No regression; gate 100 -> 102. Fulfills the user's "Tetris -> playable title screen" goal
  with a free, legal homebrew game. Crossed from "passes tests" into "runs real software".

### Note: same-suite (SameBoy's own suite, 78 ROMs) baselined at 2/78 — needs sample-accurate
  APU channels (frequency timers, duty, wave/noise output); a multi-round APU deepening.

## Round 9 — APU (sound) core (PASS 95->100)  [committed]

### What was built
- src/apu.c (new subsystem): NRxx register file + read masks, NR52 power (off clears
  registers; DMG still allows length writes while off), 512 Hz frame sequencer clocked by
  the falling edge of DIV bit 12, length counters for all 4 channels (with the trigger /
  enable "extra clock" quirk), volume envelopes, ch1 frequency sweep (with overflow disable).
- Wired apu_read/write into bus.c (FF10-FF26, FF30-FF3F) and apu_tick into cpu.c tick();
  apu_init in cpu_init_postboot (DMG post-boot register values).

### Verified (real runs)
- Blargg dmg_sound subtests are screen-output only (no serial) -> frame-hash gate (1300
  frames, verified "Passed" visually via a contact sheet, then hashed). 5/12 PASS:
  01-registers, 02-len ctr, 03-trigger, 06-overflow on trigger, 11-regs after power.
- No regression: cpu_instrs, acid2 (0/23040), full prior gate. Gate 95 -> 100 (crossed 100).

### What did NOT work / next (7 dmg_sound fail)
- 04-sweep, 05-sweep details, 07-len sweep period sync: sweep edge cases + FS-period sync.
- 08-len ctr during power: power/frame-sequencer-DIV coupling precision.
- 09/10/12 wave read/trigger/write-while-on: wave channel access quirks (reading/writing
  wave RAM while the channel is on has DMG-specific timing).
- No audio OUTPUT yet (sample generation + cpal) — the tests only check register/timing.
- Gotcha (again): adding roms/dmg_sound/ made the serial sweep pick them up (timeout fails);
  excluded roms/dmg_sound from the serial find (same as mooneye/acid2).

## Round 8 — PPU STAT mode-field timing quirk (PASS 93->95)  [committed]

### What was built
- src/ppu.c `stat_reported_mode()`: the mode field read via FF41 lags the internal mode by
  8 dots (STAT_MODE_DELAY) at the 2->3 and 3->0 boundaries. The STAT IRQ (stat_check) and
  rendering still use the real `g->mode` transitions.
- Variable mode-3 length: `mode3_end() = 80 + 172 + (SCX & 7)`.

### How it was found (systematic instrumentation)
- Confirmed first that mode timing (2@0, 3@80, 0@252) and dispatch were already correct
  (halt_ime/di/ei timing tests pass). So the intr_2 failures were a STAT-field quirk.
- Traced the test's STAT poll reads on line 0x44: mode3 read@84 saw mode3 but the test
  needs mode2 there (read@88 mode3); mode0 read@256 saw mode0 but needs mode3 (read@260
  mode0). Both = the real transition + 8 dots. Implemented exactly that.
- Tried delaying the STAT IRQ too -> broke intr_2_0_timing. Lesson: only the FIELD lags,
  not the IRQ. Reverted; +intr_2_mode0_timing, +intr_2_mode3_timing, no regression.

### Verified
- ppu cluster 3/12 -> 5/12. acid2 still 0/23040. Full gate 93 -> 95, all green.

### Still failing / next (7 ppu): hblank_ly_scx (SCX penalty added but mode-0 IRQ timing
  needs more), intr_2_mode0_timing_sprites + intr_2_oam_ok (sprite mode-3 penalty + OAM
  access timing), lcdon_timing + lcdon_write (LCD-on first-frame quirk), stat_lyc_onoff,
  vblank_stat_intr. A per-dot FIFO refactor would subsume these.

## Round 7 — MBC2 + MBC5 + MBC1 banking-bits (PASS 66->93)  [committed]

### What was built
- Rewrote src/cart.c with a per-MBC banking dispatch:
  - MBC1: store raw 5-bit BANK1 (0->1 translation moved to use-time), 2-bit BANK2, mode;
    fixes bits_bank1/bits_bank2/bits_mode/bits_ramg.
  - MBC2: address-bit-8 register select (RAMG vs ROMB), 4-bit ROM bank, built-in
    512x4-bit RAM (upper nibble reads 1).
  - MBC5: 9-bit ROM bank (low 8 @ 2000-2FFF, bit 8 @ 3000-3FFF), 4-bit RAM bank, no 0->1.
  - has_battery detection extended to MBC2/3/5 battery types.
- run_tests.sh mooneye budget 12M -> 40M.

### Verified (real runs)
- emulator-only MBC: 8/28 -> 27/28 (all MBC5, all MBC2 except... all pass; MBC1 bits all
  pass; only mbc1/multicart_rom_8Mb fails = MBC1m multicart, special).
- No regression: acceptance still 49/49, cpu_instrs/acid2 green. Gate 66 -> 93, 2.8s.

### What did NOT work / notes
- The MBC bits tests first appeared to "hang" (timeout at 12M cycles). Traced the MBC
  writes (correct banking!) and the PC (stuck in HRAM memcmp) — root cause was simply the
  cycle BUDGET: these tests legitimately run ~12.5M cycles (0x2000-address sweep x HRAM
  memcmp), and the 12M default cut them off at 99.97%. Bumped to 40M. Lesson: distinguish
  "hang" from "budget too low" by raising the budget before debugging logic.
- mbc1/multicart (MBC1m): needs detecting a multicart and remapping BANK2 into bits 4-5 of
  the ROM bank with a different mask. Deferred (1 test, special).
- Battery .sav save deferred to the interactive-frontend round (unverified I/O otherwise).
- Chose MBC over the PPU cluster this round: higher ROI (+27 vs an uncertain 0-4), lower
  risk, and a stated goal. PPU mode-timing is round 8.

## Round 6 — public release (docs + GitHub publish)  [committed + pushed]  (user-directed)

- Owner directed: create a public repo with a complete README + block diagram, upload project.
- Wrote a comprehensive README.md (overview, **Mermaid** architecture/block diagram, the
  per-M-cycle timing model with examples, the 4 test-verification strategies, build/run,
  roadmap, third-party ROM attribution) + MIT LICENSE (vendored ROMs noted under their own
  licenses). Adversarially fact-checked the README against the code via a sub-agent; fixed 3
  real inaccuracies (serial capture path, layout section, tick-before-access scope) + a stale
  main.c comment before pushing.
- Published: https://github.com/yusenthebot/gameboy-emu (PUBLIC, branch main, 6 commits, MIT,
  topics set). Verified remote tree + README integrity via the GitHub API.
- Gate unchanged (66/66) — publish round, not a test round.

## Frontier

- CURRENT CEILING: cycle-accurate CPU + OAM DMA + timer quirks; 49/66 Mooneye DMG; acid2 perfect.
  Now PUBLIC at github.com/yusenthebot/gameboy-emu. The PPU is still the weak link (fixed mode timing).
- NEXT FRONTIER (round 6): PPU mode-timing cluster (ppu/*, 8 tests) — the real remaining
  frontier. Variable mode-3 length, precise STAT mode/LYC IRQ edges, LCD-on timing. Strongly
  consider the FIFO pixel-pipeline (per-dot) refactor as the substrate — it makes mode-3
  length + STAT quirks fall out and sets up Wilbert Pol / mealybug-tearoom later.
- LADDER beyond: FIFO pixel pipeline + STAT quirks (Mooneye PPU, Wilbert Pol, mealybug) ->
  APU + cpal + Blargg dmg_sound -> MBC3/5 + RTC + .sav -> CGB mode (double speed, HDMA,
  palettes; fixes interrupt_time) -> SDL frontend + input + real games -> debugger + savestates.
- AMBITION CRITIC: CPU/DMA timing is now strong, but the PPU is still fixed-timing (acid2
  passes because it has no mid-line tricks). The mid-scanline timing tail (FIFO, mode-3
  length, STAT quirks) is the next real climb — that's where "SameBoy-class" is won.
- FRONTIER LADDER (updated): Mooneye timing/oam/ppu -> MBC2/3/5+RTC+save -> APU+cpal+
  dmg_sound -> FIFO pixel pipeline + exact mode-3 timing + STAT quirks (Mooneye PPU,
  Wilbert Pol) -> CGB mode (double speed, VRAM banks, HDMA, palettes; fixes interrupt_time)
  -> SDL frontend + input + real games (frame-hash playability) -> debugger + save-states
  + rewind.
- AMBITION CRITIC: timing is now real (the hard part has begun), but only the Blargg tier.
  Mooneye is far stricter and is the true measure of "SameBoy-class". That's the climb.
- FRONTIER LADDER (ambition x feasibility, rough order):
  1. Scanline PPU + rendering + acid2 + playable title screen.        <- round 2
  2. MBC2/3/5 + RTC + battery save (.sav) -> more games, save states.
  3. Per-M-cycle bus ticking -> mem_timing, mem_timing-2 pass.
  4. APU (4 channels) + cpal audio out; Blargg dmg_sound tests.
  5. FIFO pixel pipeline + exact mode timing + STAT quirks -> Mooneye PPU, Wilbert Pol.
  6. CGB mode (double speed, VRAM banks, palettes, HDMA).
  7. Debugger (breakpoints, mem/VRAM view), save-states, rewind/replay.
- RADICAL IDEAS WEIGHED: rewrite in Rust (deferred — net-blocked, no arch reason yet);
  SDL2 interactive front-end (deferred until there are pixels worth showing — round 2+).
- AMBITION CRITIC (what an expert would find unimpressive today): no picture at all yet;
  instruction-stepped timing is the "easy 90%". The real work is the sub-instruction tail.
