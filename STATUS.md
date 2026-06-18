# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 2 (complete, committed)
SUBSTRATE: C11 + clang
PASS COUNT: 13/13  (12 serial: cpu_instrs 11/11 + combined + instr_timing; 1 image: dmg-acid2)

CURRENT STATE:
- SM83 CPU core: full opcode set + CB, flags, interrupts, EI-delay, HALT+halt-bug, DAA.
- Scanline PPU (src/ppu.c): BG + window + sprites, all LCDC features, palettes, X/Y flip,
  8x16 sprites, sprite priority (lower X then OAM idx) + 10/line limit, BG-priority.
  Renders 160x144 shade-index framebuffer at the mode3->0 transition per line.
- PNG writer (src/png.c, stored-zlib, no deps); --frames/--png/--raw dump mode in main.
- Memory bus, MBC1 cart, timer (falling-edge + reload delay), serial capture.
- Model: instruction-stepped (subsystems ticked once per instruction).

VERIFY: `make && ./tools/run_tests.sh`   (expect PASS: 13/13, exit 0)
        acid2: ./gbemu roms/acid2/dmg-acid2.gb --frames 30 --raw /tmp/a.raw &&
               python3 tools/imgcmp.py tests/refs/dmg-acid2-ref.png /tmp/a.raw  (0 mismatches)

NEXT ROUND SEED (round 3): grow the gate + start true cycle accuracy.
  - Fetch Mooneye-GB + Blargg mem_timing / mem_timing-2; establish expanded baseline.
  - Begin per-M-cycle bus ticking (read/write tick the subsystems mid-instruction) so
    mem_timing/mem_timing-2 and Mooneye timing tests can pass. This is THE core frontier
    (the "long timing tail" the owner called out). Spike-then-migrate; keep 13/13 green.
  - Likely also: MBC2/3/5 + battery .sav (more ROMs runnable), then APU.

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
