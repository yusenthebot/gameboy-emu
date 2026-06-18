# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 1 (complete, committed)
SUBSTRATE: C11 + clang (Rust install blocked by flaky network; reconsider later)
PASS COUNT: 12/12  (cpu_instrs 11/11 + combined + instr_timing)

CURRENT STATE:
- SM83 CPU core: full opcode set + CB, flags, interrupts, EI-delay, HALT+halt-bug, DAA.
- Memory bus, MBC1 cart, timer (falling-edge + reload delay), serial capture,
  ppu_lite (free-running LY/STAT/VBlank, NO rendering yet).
- Headless harness: ROM -> serial "Passed/Failed" -> stdout, exit code.
- Model: instruction-stepped (subsystems ticked once per instruction).

VERIFY: `make && ./tools/run_tests.sh`  (expect PASS: 12/12, exit 0)
        `./gbemu roms/cpu_instrs.gb`     (expect "Passed", exit 0)

NEXT ROUND SEED (round 2): scanline PPU with real rendering.
  - Background + window + sprite (OBJ) pixel output to a 160x144 framebuffer.
  - PNG dump of frame N (tools) for visual + hash verification.
  - Fetch dmg-acid2 ROM + reference PNG, pixel-diff gate.
  - Goal: Tetris (or homebrew) reaches a stable title screen; acid2 matches.
  - Then escalate toward FIFO pixel pipeline + mode-timing for STAT/Mooneye PPU tests.

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
