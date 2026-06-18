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

## Frontier

- CURRENT CEILING: cycle-counted CPU (instruction granularity); free-running LY; no pixels.
- NEXT FRONTIER (round 2): real scanline PPU — BG/window/OBJ rendering to 160x144,
  PNG frame dump, dmg-acid2 pixel-diff gate, Tetris/homebrew title screen.
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
