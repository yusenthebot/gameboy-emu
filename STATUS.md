# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 10 (complete, committed) — playability: real game runs (libbet)
SUBSTRATE: C11 + clang
PASS COUNT: 102/102  (15 serial + acid2 + 6 framehash + 2 game[libbet title+gameplay] + 78 Mooneye)
  Round 10: a real homebrew GAME (libbet) renders its playable title screen, takes input
  (--keys), and renders actual gameplay. Fulfills the "playable title screen" goal.

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

APU (src/apu.c, round 9): NRxx register file + masks, NR52 power (off clears regs; DMG
  length-write-while-off quirk), 512Hz frame sequencer (DIV bit 12 falling edge), length
  counters (+ trigger/enable extra-clock quirk), envelope, ch1 sweep. dmg_sound subtests
  screen-output only (no serial) -> gated by frame-hash (1300 frames). PASS: 01-registers,
  02-len ctr, 03-trigger, 06-overflow on trigger, 11-regs after power (5/12).
  FAILING (7): 04-sweep, 05-sweep details, 07-len sweep period sync (sweep edge cases +
  FS-period sync), 08-len ctr during power (power/FS-DIV coupling precision), 09/10/12
  wave read/trigger/write-while-on (wave channel access quirks). No audio output yet (cpal).

PLAYABILITY (round 10): src/main.c --keys "frame:btn,..." scripted input (right/left/up/
  down/a/b/select/start/none). libbet (PinoBatch, zlib) vendored under roms/games/. Title
  screen @ frame 600 (static) + gameplay @ frame 900 after Start press, both frame-hashed.
  Proven: real game runs, MBC handling, input works, gameplay renders. uCity (CGB-only,
  MBC5 128KB) also runs and correctly shows its "GBC only" DMG-detection screen (not vendored).
  NEXT for playability: interactive window (minifb/SDL + keyboard) + cpal audio = truly
  playable by a human; more games (a real Tetris-like via free homebrew).

NEXT ROUND SEED (round 11): options — (a) interactive frontend (minifb/SDL window + keyboard
  + cpal audio) so a human can actually play; (b) more APU (dmg_sound wave quirks 09/10/12,
  sweep 04/05/07) or same-suite APU (needs sample-accurate channels); (c) PPU (sprite/OAM
  mode-3 penalties, LCD-on quirk, FIFO rewrite); (d) MBC3+RTC + battery .sav (save games).

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
