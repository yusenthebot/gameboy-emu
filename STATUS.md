# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 4 (complete, committed)
SUBSTRATE: C11 + clang
PASS COUNT: 63/63  (15 serial + acid2 img + halt_bug hash + 46 Mooneye acceptance)

CURRENT STATE:
- SM83 CPU: full opcodes+CB, per-M-cycle cycle-accurate timing (tick-before-access).
- Cycle-accurate OAM DMA (src/bus.c dma_tick): 160 M-cycles, startup delay 3, OAM locked
  to CPU during transfer, restart supported. Unlocked the whole instruction-timing cluster.
- Scanline PPU: BG+window+sprites, priorities, palettes. acid2 perfect.
- Cart ROM-only+MBC1. Timer (falling-edge + reload delay). Serial capture. PNG dump.
- Mooneye harness mode (--mooneye): LD B,B breakpoint + Fibonacci reg signature.
- Harness categories: serial / image-diff / frame-hash / mooneye.

VERIFY: `make && ./tools/run_tests.sh`   (expect PASS: 63/63, exit 0)

MOONEYE: 46/66 DMG acceptance pass. Vendored passers under roms/mooneye/ are the gate.
STILL FAILING (frontier, 20; ROMs in /tmp/gbtr_x/mooneye-test-suite, not vendored):
  - ppu/* (8): mode-timing — intr_2_mode0/mode3, lcdon_timing, hblank_ly_scx, stat_lyc_onoff,
    vblank_stat_intr, intr_2_oam_ok. Need variable mode-3 length + precise STAT. BIG PPU round.
  - timer/ (3): rapid_toggle (TAC-write glitch), tima_write_reloading, tma_write_reloading.
  - oam_dma_start, oam_dma/sources-GS (bus-conflict reads during DMA).
  - bits/unused_hwio (read masks), interrupts/ie_push (IE-overwrite cancel quirk),
    rapid_di_ei, boot_div, boot_hwio, serial/boot_sclk_align.

NEXT ROUND SEED (round 5): two tracks, pick highest ROI first —
  A) Timer quirks (timer.c): TAC-write falling-edge glitch + TIMA/TMA write-during-reload
     => rapid_toggle, tima_write_reloading, tma_write_reloading (+ likely div edge tests).
  B) PPU mode-timing: variable mode-3 length, precise STAT mode/LYC IRQ, LCD-on timing
     => the ppu/* cluster (8). Larger; may need the FIFO refactor.
  Cheap singles: unused_hwio (masks), ie_push (dispatch cancel quirk), rapid_di_ei.

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
