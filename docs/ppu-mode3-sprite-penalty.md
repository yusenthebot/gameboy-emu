# Mode-3 sprite penalty — analysis & oracle (frontier groundwork)

Mooneye `acceptance/ppu/intr_2_mode0_timing_sprites` is the gate for the pixel-FIFO
sprite mode-3 penalty. It is the highest-leverage remaining PPU test (it also unlocks
much of the Wilbert Pol `gpu/` suite). This file captures what's known so a future
round can build the simulation against a verified oracle instead of re-deriving it.

## The test

105 testcases, each places N objects on one line at given X positions and asserts the
mode-3 extension (`extra`) via the mode-0 poll. The full oracle (X-list -> extra) is
extractable from the test source with:

```sh
grep -E '^\s*testcase' intr_2_mode0_timing_sprites.s
# 'testcase E, x0, x1, ...'  => E extra cycles for objects at x0,x1,...
```

## What the data shows (single-column cases, N objects all at X with X%8 = c)

```
extra(n, c):
 n=1 : c=0..3 -> 2,  c=4..7 -> 1
 n=10: c=0,1  -> 16, c=2..7 -> 15
 n at c=0: 2,4,5,7,8,10,11,13,14,16  == floor(3n/2) + 1
```

So `extra(n,c) = floor(3n/2) + bonus(n,c)`, where `bonus = 1` iff `c < T(n)`, and the
threshold is **N-dependent**: `T(1)=4`, `T(10)=2`. A closed form does NOT generalise —
two groups at different X add a cross-group cost (5 objects at X=0 + 5 at X=160 = 17,
not 2*8). This is genuine pixel-FIFO fetcher-stall behaviour.

## Unit insight

`extra` is almost certainly **M-cycles** (the poll loop runs in M-cycles): 10 objects
extend mode 3 by ~60 dots ~= 15 M-cycles ~= the measured 16. A faithful model must
therefore compute mode-3 length in **dots** with a dot-stepped fetcher and let the
existing (calibrated) mode-0 poll convert to the measured M-cycle count — exactly how
`intr_2_mode0_timing` and the SCX tests already pass.

## Plan for a future round

Build a dot-stepped pixel-FIFO mode-3 length function (background fetcher state machine
+ per-object fetch stalls), cache it per line at the mode-2->3 transition, and feed it
into `mode3_end()`. Verify against ALL 105 oracle rows before gating. A first-attempt
closed form (`floor(3n/2)+c`) was tried in round 16 and reverted — it fits only the
cases read first, not the N-dependent threshold or cross-group cost.
