# gameboy-emu

A from-scratch, **cycle-accurate Game Boy (DMG) emulator** written in portable C11,
built incrementally toward SameBoy-class timing precision. No GPU, no heavy
dependencies — the whole core is ~1,700 lines of C and the test harness is fully
automated.

![language](https://img.shields.io/badge/language-C11-blue)
![tests](https://img.shields.io/badge/tests-1038%2F1038%20green-brightgreen)
![mooneye](https://img.shields.io/badge/Mooneye-51%2F66%20acceptance%20%2B%2027%2F28%20MBC-success)
![license](https://img.shields.io/badge/license-MIT-green)

The emphasis is **timing**, not just "the picture looks right." The CPU is stepped at
**machine-cycle granularity** — every memory access and internal cycle advances the
timer, PPU, OAM-DMA and serial subsystems at the exact point it happens — so the
emulator passes not only the easy instruction tests but the long tail of
sub-instruction timing tests.

## What it passes

| Suite | Result |
|-------|--------|
| **Blargg `cpu_instrs`** | 11/11 sub-tests → `Passed` |
| **Blargg `instr_timing`** | `Passed` |
| **Blargg `mem_timing`** | read / write / modify → `Passed` |
| **Blargg `halt_bug`** | `Passed` (HALT-bug correct) |
| **dmg-acid2** | **0 / 23040 pixel mismatches** vs the official reference |
| **cgb-acid2** (Game Boy **Color** PPU) | **0 / 23040** pixel mismatches, in color |
| **Mooneye-GB acceptance (DMG)** | **51 / 66** |
| **Mooneye-GB MBC** (MBC1/2/5) | **27 / 28** |
| **mbc3-tester** (MBC30 banking) | 0 mismatches vs reference |
| **Blargg `dmg_sound`** (APU) | **5 / 12** subtests |
| **Wilbert Pol** suite (extended PPU) | **54 / 102** DMG |
| **SameSuite** (SameBoy's suite) | 2 DMG (APU/wave) |
| **Real homebrew game** (libbet) | renders title + plays on scripted input |

The full regression gate is **1038/1038 green** (`tools/run_tests.sh`). Every check is
automated — no human in the loop, no "looks correct."

## It runs real games

Beyond the test ROMs, the emulator runs real homebrew. Here is *Libbet and the Magic
Floor* (a free zlib-licensed homebrew by PinoBatch) — its **playable title screen**, and
**actual gameplay** after the harness scripts a `Start` press (`--keys "650:start,665:none"`):

| Title screen | Gameplay (after Start) |
|:---:|:---:|
| ![libbet title](docs/screenshots/libbet-title.png) | ![libbet gameplay](docs/screenshots/libbet-gameplay.png) |

Both frames are deterministic and frame-hash–gated in the regression suite, so "it runs
real games" stays true automatically. (uCity, a CGB-only MBC5 game, also runs and correctly
shows its "this game is only for GBC" DMG-detection screen.)

## Architecture

```mermaid
flowchart TB
    subgraph H["main.c — automated test harness"]
        direction LR
        H1["serial<br/>read Passed/Failed"]
        H2["frame<br/>dump PNG / raw"]
        H3["mooneye<br/>LD B,B + reg signature"]
    end

    H -->|"cpu_step()"| CPU

    subgraph CPU["cpu.c — SM83 core"]
        direction TB
        C1["fetch / decode / execute<br/>256 base + 256 CB opcodes"]
        C2["interrupts · EI-delay<br/>HALT + halt-bug · DAA"]
        C3["tick() — advances the clock<br/>per machine cycle"]
    end

    CPU -->|"bus_read / bus_write"| BUS
    CPU -->|"tick(4) every M-cycle"| SUB

    subgraph BUS["bus.c — memory map + I/O"]
        B1["ROM · VRAM · WRAM<br/>OAM · HRAM · echo"]
        B2["I/O dispatch + HWIO read masks"]
    end

    subgraph SUB["subsystems clocked inside tick()"]
        direction LR
        T1["timer.c<br/>DIV/TIMA/TMA/TAC"]
        T2["ppu.c<br/>BG · window · sprites"]
        T3["OAM DMA<br/>160 M-cycles, OAM lock"]
        T4["serial.c<br/>link port"]
        T5["apu.c<br/>4 channels, frame seq"]
    end

    BUS --> CART["cart.c<br/>MBC1/2/3/5 + RAM/RTC"]
    T2 --> FB["framebuffer 160×144"]
    FB --> PNG["png.c<br/>grayscale PNG"]
    CART -.-> BUS
    SUB -.->|"request IRQ (IF)"| CPU
```

### Components

| File | Lines | Responsibility |
|------|------:|----------------|
| `src/cpu.c` | 451 | SM83 core: every opcode, exact flags, interrupt dispatch, the per-M-cycle clock |
| `src/ppu.c` | 244 | Scanline PPU: BG, window, sprites, priorities, palettes, mode/STAT timing → 160×144 |
| `src/cart.c` | 208 | Cartridge loading, header parsing, MBC1/2/3/5 banking + RTC + external RAM |
| `src/bus.c` | 119 | System memory map, I/O register dispatch, cycle-accurate OAM DMA |
| `src/png.c` | 112 | Dependency-free grayscale PNG writer (for frame dumps / diffs) |
| `src/apu.c` | 330 | Sound: NRxx registers, frame sequencer, length/envelope/sweep + square/wave/noise synthesis to 48kHz PCM |
| `src/timer.c` | 103 | DIV/TIMA/TMA/TAC with falling-edge detection and the reload-window quirks |
| `src/main.c` | 145 | Headless entry point: serial / frame-dump / mooneye / save-state modes |
| `src/play.c` | 130 | Interactive SDL2 frontend (`gbplay`): window, keyboard → joypad, save-state hotkeys |
| `src/state.c` | 60 | Save-states: full machine snapshot to a file + restore |
| `src/disasm.c` | 95 | SM83 disassembler (algorithmic LD/ALU/CB blocks + a table for the rest) |
| `src/debug.c` | 90 | CLI debugger REPL: regs/step/break/continue/mem/disasm |
| `src/serial.c` | 50 | Link-port serial capture (the Blargg `Passed/Failed` channel) |
| `src/gb.h` | 151 | Shared types and the single `GB` machine-state struct |

## The cycle model

Round 1 of this project used the common "instruction-stepped" approximation: execute a
whole instruction, then advance the subsystems by its total cycle count. That passes
`cpu_instrs` and `instr_timing` but **fails every sub-instruction timing test**.

The core was then rewritten so that time advances **inside** each instruction. A single
`tick(g, 4)` advances the timer, PPU, OAM-DMA and serial by one machine cycle, and each
data access ticks the clock right before it reads or writes — *tick-before-access*, so
the access lands on the last T-cycle of its machine cycle, matching the hardware:

```
LD A,(HL)      fetch ──tick(4)── read(HL) ──tick(4)──        = 8 T-cycles
PUSH BC        fetch ──tick(4)── internal ──tick(4)── wr hi ──tick(4)── wr lo ──tick(4)──  = 16
```

Because the budget *emerges* from the ticks rather than a lookup table, the timing is
correct by construction. This is what lets the emulator pass Blargg `mem_timing` and the
Mooneye instruction-timing cluster (`push_timing`, `call_timing`, `ret_timing`, …), and
it is the reason the cycle-accurate **OAM DMA** — which Mooneye uses as a timing probe —
unlocks so many tests at once.

## Build & run

The headless emulator (`gbemu`) needs only a C11 compiler — no external libraries.

```sh
make                                  # builds ./gbemu (headless)

# run a serial test ROM (exit 0 = Passed)
./gbemu roms/cpu_instrs.gb

# render N frames and dump a PNG you can open
./gbemu roms/acid2/dmg-acid2.gb --frames 30 --png out.png

# run a Mooneye test (LD B,B breakpoint + register signature)
./gbemu roms/mooneye/acceptance/timer/tim00.gb --mooneye

# snapshot the whole machine and resume bit-identically
./gbemu game.gb --frames 100 --save-state s.gss
./gbemu game.gb --frames 200 --load-state s.gss --png out.png

# the full regression gate
./tools/run_tests.sh                  # -> PASS: 1038/1038
```

### Play it (interactive SDL2 frontend)

```sh
make play                             # builds ./gbplay (needs SDL2)
./gbplay roms/games/libbet/libbet.gb
```

Controls: **arrows** = D-pad, **Z** = A, **X** = B, **Enter** = Start,
**Shift** = Select, **F5** = quick-save, **F9** = load, **Backspace** = rewind, **Esc** = quit.

Audio plays through SDL (square, wave and noise channels mixed with NR50/NR51 panning).

### Debug it

```sh
# step, breakpoints, memory, and a full SM83 disassembler
printf 'd 0x100 8\nbreak 0x150\ncont\nregs\nq\n' | ./gbemu rom.gb --debug
```

## How tests are verified (no "looks right")

The harness uses four fully-deterministic strategies, one per ROM category:

- **serial** — the ROM writes `Passed`/`Failed` to the link port; the emulator captures
  those bytes (mirroring them to stdout for live progress) and sets its exit code from the
  result (Blargg convention).
- **image-diff** — dmg-acid2 renders a fixed frame; `tools/imgcmp.py` decodes the
  official reference PNG and compares pixel-for-pixel (0 mismatches required).
- **frame-hash** — for screen-only ROMs (e.g. `halt_bug`), a SHA-256 of a deterministic
  frame is compared against a verified-once hash.
- **mooneye** — the ROM ends with `LD B,B` (a software breakpoint) and leaves a
  Fibonacci signature in the registers (`B=3 C=5 D=8 E=13 H=21 L=34` = pass).

## Project layout

```
src/                     cpu, ppu, bus, cart, timer, apu, serial, png, main
tools/                   run_tests.sh (the gate) · imgcmp.py (PNG diff)
roms/                    freely-redistributable test ROMs (Blargg, dmg-acid2, Mooneye)
tests/refs/              reference images for the image-diff gate
STATUS.md, progress.md   current state + full round-by-round build log
```

## Status & roadmap

**Done:** full SM83 core · per-M-cycle timing · scanline PPU (acid2-perfect) · MBC1/2/5 ·
cycle-accurate OAM DMA · timer quirks · APU register/length/envelope/sweep core ·
runs real homebrew games (input via `--keys`) · headless test harness.

**Next (the remaining timing tail):**

- **PPU mode timing** — variable mode-3 length, precise STAT/LYC interrupt edges, LCD-on
  timing. Likely a FIFO pixel-pipeline (per-dot) refactor — this is the next major round.
- **APU** — the wave-channel access quirks, sweep edge cases, and actual audio output (cpal).
- **MBC3 + RTC**, battery-backed `.sav` persistence, and the MBC1 multicart variant.
- **CGB mode** — double speed, VRAM banks, palettes, HDMA.
- **Tooling** — a debugger, save-states, and rewind.

See `STATUS.md` for the current state and `progress.md` for the full round-by-round log.

## Third-party test ROMs

The `roms/` and `tests/refs/` directories vendor freely-redistributable test ROMs so the
gate is self-contained and reproducible offline:

- **Blargg's test suite** — freely redistributable (Shay Green).
- **Mooneye Test Suite** — MIT, © Joonas Javanainen (Gekkio). See
  `roms/mooneye/ATTRIBUTION.txt`.
- **dmg-acid2** — MIT, © Matt Currie.

Each remains under its own upstream license; they are included for convenience and are
not part of this project's own copyright.

## License

The emulator source (everything under `src/`, `tools/`, `Makefile`) is released under the
**MIT License** — see [LICENSE](LICENSE). Vendored test ROMs are covered by their own
licenses as noted above.
