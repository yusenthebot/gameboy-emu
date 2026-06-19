# APU wave channel (CH3) — groundwork (round 18 research)

Targets: Blargg `dmg_sound` 09 (wave read while on), 10 (trigger while on), 12 (write
while on). All need the CH3 sample-playback timing + the DMG wave-RAM access window.

## Spec (from `gh api repos/gbdev/pandocs/.../Audio_details.md` + `Audio_Registers.md`)

- CH3 has a 32-step "sample index". The period divider runs once per **two dots** and
  reloads with `2048 - period_value` (period = NR33 | (NR34&7)<<8); each reload advances
  the index and reads a wave-RAM byte (`wave[index >> 1]`). Sample rate = 2097152/(2048-p).
- On trigger: sample #0 is skipped (first byte read is index 1); the first byte is NOT
  loaded on trigger, so the sample buffer keeps the previous value. There is a known
  trigger -> first-read delay that is the crux of the calibration.
- **DMG wave-RAM access while CH3 is active**: the CPU can access wave RAM only on the
  same dot CH3 reads it; then the access hits the byte CH3 is reading (regardless of the
  address). Otherwise reads return 0xFF and writes are ignored. (This is the 09/12 test.)
- **Trigger corruption (10)**: triggering CH3 while it is reading a sample byte rewrites
  the first 4 bytes of wave RAM — byte 0 = the byte read if it was bytes 0-3, else bytes
  0-3 = the aligned 4-byte group (4-7, 8-11, or 12-15) the read was in.

## Round-18 attempt + what blocked it

Implemented the core (frequency timer, sample index, the access window, trigger reset)
in apu.c/gb.h. No regression (dmg_sound 01/02/03/06/11 frame-hashes unchanged), but 09/10/12
still FAIL. Instrumentation showed CH3 *is* stepping (sample index advances), yet every
wave-RAM read during test 09 lands at `timer = full period, pos = 0` (the freshly-triggered
state, window closed -> 0xFF). The test re-triggers around each read, so passing it needs
the exact trigger->first-read offset and period-divider phase. Reverted (won't ship
all-0xFF blocking that's unverified against the test).

## Next-round plan

The wave tests are Blargg binaries (no clean .s oracle like intr_2_mode0_timing_sprites had),
so calibrate empirically: re-apply the core, then sweep the trigger->first-read delay (and
the access-window width) while rendering the 09 result screen, until the printed grid matches
a pass. Cross-check 12 (write) shares the window; 10 needs the corruption rule above. The
core (sample stepping) is also the basis for real audio output (cpal) later. Alternatively
pivot to a more tractable tail (hblank_ly_scx mode-0-IRQ calibration, MBC3+.sav).
