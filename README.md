# gbemu — a Game Boy (DMG) emulator in C

A from-scratch Game Boy emulator built incrementally toward SameBoy-level cycle
accuracy. Round 1 is a complete, instruction-stepped SM83 CPU core that passes
Blargg's `cpu_instrs` and `instr_timing`.

## Build & test

```sh
make                      # builds ./gbemu (clang, C11)
./gbemu roms/cpu_instrs.gb
./tools/run_tests.sh      # regression harness, prints PASS count
```

## Status

See `STATUS.md` (current state + next round) and `progress.md` (full log + frontier).

| Area            | State                                                        |
|-----------------|-------------------------------------------------------------|
| CPU (SM83)      | Complete: all opcodes, flags, interrupts, HALT-bug, DAA      |
| Timer           | DIV/TIMA/TMA/TAC, falling-edge + reload delay                |
| Serial          | Headless capture (Passed/Failed to stdout)                  |
| Cartridge       | ROM-only + MBC1                                              |
| PPU             | ppu_lite (free-running LY/STAT/VBlank, no rendering yet)     |
| Test ROMs       | cpu_instrs 11/11, instr_timing — 12/12 green                |

## Layout

```
src/        cpu, bus, cart, timer, serial, ppu_lite, main
tools/      run_tests.sh (regression gate)
roms/       free test ROMs (Blargg)
docs/       design notes
```

Test ROMs are Blargg's freely redistributable GB test suite.
