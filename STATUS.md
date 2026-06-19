# STATUS

GOAL: Build a cycle-accurate Game Boy (DMG/CGB) emulator in C, climbing toward
SameBoy-level T-cycle precision. Gate metric = test-ROM pass count, must strictly
increase each round. (Full goal in the /loop prompt.)

ROUND: 20 (complete, committed + pushed) — INTERACTIVE PLAYABLE FRONTEND (gbplay, +1)
SUBSTRATE: C11 + clang  (gbemu headless harness + gbplay SDL2 frontend)
PASS COUNT: 123/123  (15 serial + acid2 + 9 framehash + 2 game + 3 savestate + 1 frontend + 92 Mny/WP/SS)
  Round 20: built src/play.c — SDL2 window (scaled, DMG green palette), keyboard->joypad
  (arrows/Z/X/Enter/Shift), F5/F9 quick save/load-state, ~59.7fps pacing. Makefile splits
  gbemu (gate harness) + gbplay (SDL2, pre-approved dep). Verified headlessly via SDL dummy
  video: gbplay drives the engine to frames BIT-IDENTICAL to gbemu (gate test, frames 60/300/600).
  The live window/audio runs on a real display: `make play && ./gbplay roms/games/libbet/libbet.gb`.
  Audio (SDL_audio + APU sample gen) is the next layer. Fulfills "能玩" (human-playable).

ROUND: 19 (complete, committed + pushed) — SAVE-STATES (new dimension, +3)
  Round 19: full GB snapshot (src/state.c), --save-state/--load-state, +3 determinism tests.

ROUND: 18 (complete, committed local) — APU wave-channel research (gate FLAT, honest)
SUBSTRATE: C11 + clang
PASS COUNT: 119/119  (gate FLAT — no clean +1 landed; researched two hard frontiers)
  Round 18: attacked the APU wave channel (dmg_sound 09/10/12). Got the exact DMG spec via
  gh api (sample stepping, the wave-RAM access window, trigger corruption). Implemented the
  core (no regression on passing dmg_sound) but 09/10/12 still fail: the test re-triggers
  around each read and needs the exact trigger->first-read sub-cycle offset, and the wave
  tests are Blargg binaries with NO clean .s oracle (unlike the FIFO). Reverted the unverified
  blocking. Also looked at hblank_ly_scx (mode-0 IRQ vs +8) — also deep calibration. Saved
  docs/apu-wave-channel.md. Not pushed (flat round). 2nd flat round on the genuinely-hard tail.

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

PPU cluster: 6/12 (+stat_lyc_onoff round 11). Round-11 PPU quirks added (ppu.c): LCD-off
  freezes the LY=LYC coincidence flag (g->ly_coin) + STAT mode reads 0; first frame after
  LCD-on reads mode 0 during LY=0's OAM scan (g->lcd_on_frame); the STAT interrupt line is
  now GB state (g->stat_line) holding its frozen value across LCD off/on so an LCD-on
  coincidence only re-fires on a genuine rising edge; LCD-on evaluates the coincidence IRQ
  immediately. These set up lcdon_timing/lcdon_write (still failing — need full first-frame
  mode-3 timing) for a future round.
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

WILBERT POL mining: 54/102 DMG pass overall (ED breakpoint). The new value is acceptance/gpu/
  (47 tests, 11 pass) — vendored the 6 SCX ones. The other ~36 gpu fail (sprite/OAM mode-3
  penalties, lcdon, window timing — same PPU frontier). ROMs in /tmp/gbtr_x/mooneye-test-suite-wilbertpol.

PPU remaining (5/12 fail): hblank_ly_scx (mode-0 IRQ timing vs +8 field offset — investigate
  whether the mode-0 STAT IRQ needs the +8 delay while mode-2 IRQ stays real), lcdon_timing/
  write (first-frame mode-3 timing), intr_2_mode0_timing_sprites (sprite mode-3 penalty = FIFO
  fetcher sim; data in /tmp/ppu_src/intr_2_mode0_timing_sprites.s), vblank_stat_intr.

REMAINING HARD TAIL (all +1-2, real engineering): sprite mode-3 penalty (FIFO fetcher sim —
  highest leverage, unlocks intr_2_mode0_sprites + many wilbertpol gpu), APU wave channel
  (dmg_sound 09/10/12 — freq timer + DMG wave-RAM access window), lcdon_timing/write (first
  -frame mode-3), hblank_ly_scx (mode-0 IRQ +8?), boot_div (post-boot DIV timing), rapid_toggle.

NEXT ROUND SEED (round 21): frontend video done -> AUDIO is the natural next layer (decide
  autonomously, don't ask — [[loop-full-autonomy]]):
  (1) AUDIO: generate APU samples (ch1/2 square via duty+freq timer, ch3 wave, ch4 noise LFSR)
      mixed to a buffer; feed SDL_audio in gbplay. Verifiable headlessly: hash the generated
      sample buffer for a fixed run (deterministic). Big "APU 声音" win + makes gbplay fully play.
  (2) REWIND: ring-buffer of save-states in gbplay (hold a key to step back). Verifiable round-trip.
  (3) MBC3 + battery .sav — broadens games; .sav round-trip gate-verifiable.
  (4) A CLI debugger (breakpoints/step/mem-dump) — the "调试器" goal; verifiable.
  Timing tail (sweep/wave/hblank/lcdon) only if a clean approach appears (sweep needs Blargg src).
  Recommend (1) audio (completes the playable experience + APU goal, headlessly verifiable).

GATES (pause + ask owner): new external dep beyond pre-approved set; any push/publish;
  changing public data formats. Pre-approved: clang, sdl2/minifb, cpal, free test ROMs.

RESUME: read this file, progress.md, `git log --oneline -10`, then ORIENT.
