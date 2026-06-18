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

## Frontier

- CURRENT CEILING: cycle-accurate CPU + OAM DMA; 46/66 Mooneye DMG acceptance; acid2 perfect.
  This is genuinely into SameBoy-tier territory for CPU/DMA timing.
- NEXT FRONTIER (round 5): (A) timer quirks (TAC-write glitch + reload-window writes, 3
  tests, small) and/or (B) PPU mode-timing cluster (8 tests, larger — variable mode-3
  length + precise STAT, likely the FIFO pixel-pipeline refactor). Cheap singles: unused_hwio,
  ie_push, rapid_di_ei.
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
