# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 8 (complete, committed) — PPU STAT mode-field timing
SUBSTRATE: C11 + clang
PASS COUNT: 95/95  (15 serial + acid2 + halt_bug + 78 Mooneye [51 acceptance + 27 emulator-only])
  Round 8: cracked the DMG STAT mode-field-read quirk (+intr_2_mode0_timing, intr_2_mode3_timing).

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

PPU cluster: 5/12 pass now (+intr_2_mode0_timing, intr_2_mode3_timing this round).
  KEY FINDING (ppu.c): the STAT mode FIELD read via FF41 lags the internal mode by 8 dots
  at the 2->3 and 3->0 boundaries (stat_reported_mode, STAT_MODE_DELAY=8); the STAT IRQ
  and rendering keep the REAL transitions (delaying the IRQ broke intr_2_0_timing). Also
  added variable mode-3 length = 172 + (SCX & 7).
  STILL FAILING (7): hblank_ly_scx (mode-0 IRQ + SCX interaction — SCX penalty added but
  not enough), intr_2_mode0_timing_sprites + intr_2_oam_ok (sprite mode-3 penalty + OAM
  access timing), lcdon_timing + lcdon_write (LCD-on first-frame quirk: first frame short,
  mode timing differs), stat_lyc_onoff (LYC IRQ toggling), vblank_stat_intr (vblank/STAT).
  These need: sprite/OAM mode-3 penalties, the LCD-enable first-frame model, LYC edge
  details. A per-dot FIFO refactor would make them fall out — the next big PPU push.

MBC: emulator-only 27/28 (only mbc1/multicart fails). Battery .sav DEFERRED to frontend.

NEXT ROUND SEED (round 9): continue PPU — pick (a) sprite/OAM mode-3 penalties for
  intr_2_mode0_timing_sprites + intr_2_oam_ok, or (b) the LCD-on first-frame quirk for
  lcdon_timing/lcdon_write, or (c) escalate to the FIFO per-dot rewrite (sets up all of
  them + Wilbert Pol / mealybug). Alternatively pivot to APU (new subsystem, Blargg
  dmg_sound) or the interactive frontend + a homebrew game (the Tetris-title-screen goal).

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
