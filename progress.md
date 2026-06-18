# Progress Log

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
