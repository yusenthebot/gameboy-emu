# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 3 (complete, committed)
SUBSTRATE: C11 + clang
PASS COUNT: 17/17  (15 serial + 1 image[acid2] + 1 framehash[halt_bug]; interrupt_time SKIP=CGB)

CURRENT STATE:
- SM83 CPU: full opcode set + CB, flags, interrupts, EI-delay, HALT+halt-bug, DAA.
  *** Per-M-cycle (cycle-accurate) timing: every memory access / internal M-cycle ticks
      the timer/PPU/serial by 4T at the point it happens (tick-before-access). ***
- Scanline PPU (src/ppu.c): BG+window+sprites, priorities, 10/line, palettes. acid2 perfect.
- Cart: ROM-only + MBC1. Timer (falling-edge + reload delay). Serial capture. PNG dump.
- Harness (tools/run_tests.sh): serial + image-diff + frame-hash categories.

VERIFY: `make && ./tools/run_tests.sh`   (expect PASS: 17/17, exit 0)

PASSING TIMING TESTS: cpu_instrs 11/11, instr_timing, mem_timing 01/02/03, halt_bug(screen).
KNOWN-FAIL/SKIP: interrupt_time (CGB-oriented). mem_timing-2 combined not fetched (mirror
  truncates large file). Mooneye suite not yet integrated.

NEXT ROUND SEED (round 4): integrate Mooneye-GB suite + deepen timing quirks.
  - Add a Mooneye harness mode: detect test end (LD B,B = 0x40 software breakpoint) and
    check the Fibonacci register signature (B=3 C=5 D=8 E=13 H=21 L=34 = pass).
  - Fetch Mooneye ROMs (try c-sp/gameboy-test-roms release zip, or Gekkio mirror).
    Establish a Mooneye baseline, then fix high-impact quirks: timer (div_write, tima
    reload edge cases), interrupt dispatch cancel quirk, OAM DMA timing.
  - Stretch: MBC3 + battery .sav (broaden game support); start APU (Blargg dmg_sound).

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
