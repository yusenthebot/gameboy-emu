# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 7 (complete, committed) — MBC2 + MBC5 + MBC1 banking-bits
SUBSTRATE: C11 + clang
PASS COUNT: 93/93  (15 serial + acid2 img + halt_bug hash + 76 Mooneye [49 acceptance + 27 emulator-only MBC])
  Round 6 was the user-directed publish round (docs + public repo). Round 7 = MBC support.

PUBLISHED: https://github.com/yusenthebot/gameboy-emu (PUBLIC, branch main, MIT).
  Remote tracks origin/main. README has a Mermaid architecture diagram. Future rounds:
  commit locally as usual; push to origin at milestones or when the owner asks (pushing is
  an outward gate — don't force-push; the repo is public so be deliberate).

CURRENT STATE:
- SM83 CPU: full opcodes+CB, per-M-cycle cycle-accurate timing (tick-before-access).
- Cycle-accurate OAM DMA (src/bus.c dma_tick): 160 M-cycles, startup delay 3, OAM locked
  to CPU during transfer, restart supported. Unlocked the whole instruction-timing cluster.
- Scanline PPU: BG+window+sprites, priorities, palettes. acid2 perfect.
- Cart ROM-only+MBC1. Timer (falling-edge + reload delay). Serial capture. PNG dump.
- Mooneye harness mode (--mooneye): LD B,B breakpoint + Fibonacci reg signature.
- Harness categories: serial / image-diff / frame-hash / mooneye.

VERIFY: `make && ./tools/run_tests.sh`   (expect PASS: 63/63, exit 0)

MOONEYE: 49/66 DMG acceptance pass. Vendored passers under roms/mooneye/ are the gate.
STILL FAILING (frontier, 17; ROMs in /tmp/gbtr_x/mooneye-test-suite, not vendored):
  - ppu/* (8): mode-timing — intr_2_mode0/mode3, lcdon_timing, hblank_ly_scx, stat_lyc_onoff,
    vblank_stat_intr, intr_2_oam_ok. Need variable mode-3 length + precise STAT. BIG PPU round.
  - oam_dma_start + oam_dma/sources-GS: precise DMA lock-start cycle + bus-conflict reads
    (CPU reads during DMA return the byte being copied). Needs a fuller DMA-conflict model.
  - timer/rapid_toggle (sub-cycle TAC-disable glitch count precision).
  - interrupts/ie_push (IE-overwrite cancel quirk: intricate; vector sampled mid-dispatch),
    rapid_di_ei, boot_div, boot_hwio, serial/boot_sclk_align.

MBC: emulator-only 27/28 (only mbc1/multicart_rom_8Mb fails — MBC1m needs special
  multicart wiring detection). Mooneye budget bumped to 40M in run_tests.sh (MBC bits
  tests run ~12.5M cycles — the old 12M budget cut them off at 99.97%, a budget bug not
  a logic bug). Battery .sav save DEFERRED to pair with the interactive frontend.

NEXT ROUND SEED (round 8): PPU mode-timing cluster (ppu/*, 9 tests). This is the real
  remaining frontier and the user's emphasis. Likely needs: variable mode-3 length (depends
  on SCX fine-scroll + sprite count + window), precise STAT mode/LYC IRQ edges, LCD-on
  timing (first frame after enable is short / mode quirks). Strongly consider the FIFO
  pixel-pipeline refactor (per-dot rendering) as the substrate — it makes mode-3 length and
  STAT quirks fall out naturally and sets up Wilbert Pol / mealybug later.
  Secondary cheap-ish: oam_dma_start/sources (DMA-conflict model), ie_push, rapid_di_ei.

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
