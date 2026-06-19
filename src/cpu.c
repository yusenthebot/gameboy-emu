/* cpu.c - SM83 CPU core (per-M-cycle / cycle-accurate timing).
 *
 * Every memory access and every internal machine cycle advances the
 * timer/PPU/serial by 4 T-cycles AT THE POINT IT HAPPENS (tick-before-
 * access: the access lands on the last T-cycle of its M-cycle). This
 * gives correct sub-instruction memory-access timing (Blargg mem_timing)
 * rather than the round-1/2 model that ticked once per instruction.
 *
 * Cycle budget per opcode therefore emerges from the ticks, not a return
 * value: instr_timing totals stay exact while accesses now land on the
 * right cycle within the instruction.
 */
#include "gb.h"

#define IF_REG 0x0F

/* ---- register pair access ---- */
static inline u16 rAF(GB *g) { return ((u16)g->a << 8) | (g->f & 0xF0); }
static inline u16 rBC(GB *g) { return ((u16)g->b << 8) | g->c; }
static inline u16 rDE(GB *g) { return ((u16)g->d << 8) | g->e; }
static inline u16 rHL(GB *g) { return ((u16)g->h << 8) | g->l; }
static inline void wAF(GB *g, u16 v) { g->a = v >> 8; g->f = v & 0xF0; }
static inline void wBC(GB *g, u16 v) { g->b = v >> 8; g->c = v & 0xFF; }
static inline void wDE(GB *g, u16 v) { g->d = v >> 8; g->e = v & 0xFF; }
static inline void wHL(GB *g, u16 v) { g->h = v >> 8; g->l = v & 0xFF; }

static inline void set_flag(GB *g, u8 mask, bool on) {
    if (on) g->f |= mask; else g->f &= ~mask;
    g->f &= 0xF0;
}
static inline bool flag(GB *g, u8 mask) { return (g->f & mask) != 0; }

/* ---- the one place time advances ---- */
static inline void tick(GB *g, int t) {
    /* PPU/APU run off the crystal -> half rate in double-speed; DIV/TIMA, OAM DMA
     * and serial are CPU-clocked (full t). Original tick order preserved. */
    int rt = g->double_speed ? (t >> 1) : t;
    g->cycles += t;
    g->sys_cycles += rt;              /* crystal time: tracks LCD/audio frames */
    timer_tick(g, t);
    ppu_tick(g, rt);
    dma_tick(g, t);
    apu_tick(g, rt);
    serial_tick(g, t);
}

/* ---- bus helpers: each memory M-cycle ticks 4 (access on last T) ---- */
static inline u8 rd(GB *g, u16 a) { tick(g, 4); return bus_read(g, a); }
static inline void wr(GB *g, u16 a, u8 v) { tick(g, 4); bus_write(g, a, v); }
static inline u8 imm8(GB *g) { return rd(g, g->pc++); }
static inline u16 imm16(GB *g) {
    u16 lo = imm8(g);
    u16 hi = imm8(g);
    return lo | (hi << 8);
}

/* register index map: 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A */
static u8 reg_get(GB *g, int i) {
    switch (i) {
        case 0: return g->b; case 1: return g->c;
        case 2: return g->d; case 3: return g->e;
        case 4: return g->h; case 5: return g->l;
        case 6: return rd(g, rHL(g)); default: return g->a;
    }
}
static void reg_set(GB *g, int i, u8 v) {
    switch (i) {
        case 0: g->b = v; break; case 1: g->c = v; break;
        case 2: g->d = v; break; case 3: g->e = v; break;
        case 4: g->h = v; break; case 5: g->l = v; break;
        case 6: wr(g, rHL(g), v); break; default: g->a = v; break;
    }
}

/* ---- ALU (flag-exact; unchanged from round 1) ---- */
static u8 alu_inc(GB *g, u8 v) {
    u8 r = v + 1;
    set_flag(g, FLAG_Z, r == 0); set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, (v & 0x0F) == 0x0F);
    return r;
}
static u8 alu_dec(GB *g, u8 v) {
    u8 r = v - 1;
    set_flag(g, FLAG_Z, r == 0); set_flag(g, FLAG_N, true);
    set_flag(g, FLAG_H, (v & 0x0F) == 0x00);
    return r;
}
static void alu_add(GB *g, u8 v) {
    u16 r = g->a + v;
    set_flag(g, FLAG_H, ((g->a & 0xF) + (v & 0xF)) > 0xF);
    set_flag(g, FLAG_C, r > 0xFF);
    g->a = (u8)r;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, false);
}
static void alu_adc(GB *g, u8 v) {
    u8 c = flag(g, FLAG_C) ? 1 : 0;
    u16 r = g->a + v + c;
    set_flag(g, FLAG_H, ((g->a & 0xF) + (v & 0xF) + c) > 0xF);
    set_flag(g, FLAG_C, r > 0xFF);
    g->a = (u8)r;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, false);
}
static void alu_sub(GB *g, u8 v) {
    set_flag(g, FLAG_H, (g->a & 0xF) < (v & 0xF));
    set_flag(g, FLAG_C, g->a < v);
    g->a = g->a - v;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, true);
}
static void alu_sbc(GB *g, u8 v) {
    u8 c = flag(g, FLAG_C) ? 1 : 0;
    int r = g->a - v - c;
    set_flag(g, FLAG_H, ((g->a & 0xF) - (v & 0xF) - c) < 0);
    set_flag(g, FLAG_C, r < 0);
    g->a = (u8)r;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, true);
}
static void alu_and(GB *g, u8 v) {
    g->a &= v;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, true); set_flag(g, FLAG_C, false);
}
static void alu_or(GB *g, u8 v) {
    g->a |= v;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, false); set_flag(g, FLAG_C, false);
}
static void alu_xor(GB *g, u8 v) {
    g->a ^= v;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, false); set_flag(g, FLAG_C, false);
}
static void alu_cp(GB *g, u8 v) {
    set_flag(g, FLAG_H, (g->a & 0xF) < (v & 0xF));
    set_flag(g, FLAG_C, g->a < v);
    set_flag(g, FLAG_Z, g->a == v); set_flag(g, FLAG_N, true);
}
static void add_hl(GB *g, u16 v) {
    u16 hl = rHL(g);
    u32 r = (u32)hl + v;
    set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, ((hl & 0xFFF) + (v & 0xFFF)) > 0xFFF);
    set_flag(g, FLAG_C, r > 0xFFFF);
    wHL(g, (u16)r);
}
static u16 add_sp_e(GB *g, i8 e) {
    u16 sp = g->sp;
    u8 ue = (u8)e;
    set_flag(g, FLAG_Z, false); set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, ((sp & 0xF) + (ue & 0xF)) > 0xF);
    set_flag(g, FLAG_C, ((sp & 0xFF) + ue) > 0xFF);
    return (u16)(sp + (i16)e);
}
static void daa(GB *g) {
    int a = g->a, adjust = 0;
    bool c = flag(g, FLAG_C);
    if (!flag(g, FLAG_N)) {
        if (flag(g, FLAG_H) || (a & 0x0F) > 9) adjust |= 0x06;
        if (c || a > 0x99) { adjust |= 0x60; c = true; }
        a += adjust;
    } else {
        if (flag(g, FLAG_H)) adjust |= 0x06;
        if (c) adjust |= 0x60;
        a -= adjust;
    }
    g->a = (u8)a;
    set_flag(g, FLAG_Z, g->a == 0); set_flag(g, FLAG_H, false);
    set_flag(g, FLAG_C, c);
}

/* ---- CB rotate/shift/bit (unchanged) ---- */
static u8 cb_rlc(GB *g, u8 v) { u8 c = v >> 7; v = (v << 1) | c;
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }
static u8 cb_rrc(GB *g, u8 v) { u8 c = v & 1; v = (v >> 1) | (c << 7);
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }
static u8 cb_rl(GB *g, u8 v) { u8 oc = flag(g, FLAG_C) ? 1 : 0; u8 c = v >> 7; v = (v << 1) | oc;
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }
static u8 cb_rr(GB *g, u8 v) { u8 oc = flag(g, FLAG_C) ? 1 : 0; u8 c = v & 1; v = (v >> 1) | (oc << 7);
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }
static u8 cb_sla(GB *g, u8 v) { u8 c = v >> 7; v <<= 1;
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }
static u8 cb_sra(GB *g, u8 v) { u8 c = v & 1; v = (v >> 1) | (v & 0x80);
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }
static u8 cb_swap(GB *g, u8 v) { v = (v >> 4) | (v << 4);
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, 0); return v; }
static u8 cb_srl(GB *g, u8 v) { u8 c = v & 1; v >>= 1;
    set_flag(g, FLAG_Z, v == 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return v; }

static void do_cb(GB *g) {
    u8 cb = imm8(g);
    int idx = cb & 7, sub = cb >> 3;
    u8 v = reg_get(g, idx);              /* (HL) read ticks 4 */
    if (sub < 8) {
        switch (sub) {
            case 0: v = cb_rlc(g, v); break;  case 1: v = cb_rrc(g, v); break;
            case 2: v = cb_rl(g, v); break;   case 3: v = cb_rr(g, v); break;
            case 4: v = cb_sla(g, v); break;  case 5: v = cb_sra(g, v); break;
            case 6: v = cb_swap(g, v); break; case 7: v = cb_srl(g, v); break;
        }
        reg_set(g, idx, v);              /* (HL) write ticks 4 */
    } else if (sub < 16) {
        int b = sub - 8;
        set_flag(g, FLAG_Z, !((v >> b) & 1));
        set_flag(g, FLAG_N, false); set_flag(g, FLAG_H, true);
    } else if (sub < 24) {
        reg_set(g, idx, v & ~(1 << (sub - 16)));
    } else {
        reg_set(g, idx, v | (1 << (sub - 24)));
    }
}

/* ---- stack ---- */
static void push16(GB *g, u16 v) {
    g->sp--; wr(g, g->sp, v >> 8);
    g->sp--; wr(g, g->sp, v & 0xFF);
}
static u16 pop16(GB *g) {
    u16 lo = rd(g, g->sp++);
    u16 hi = rd(g, g->sp++);
    return lo | (hi << 8);
}

void cpu_request_interrupt(GB *g, u8 mask) {
    g->io[IF_REG] |= (mask & 0x1F);
}

static void service_interrupt(GB *g) {
    tick(g, 4); tick(g, 4);              /* 2 internal M-cycles */
    g->ime = false;
    u8 pend = g->ie & g->io[IF_REG] & 0x1F;
    int bit = 0;
    while (bit < 5 && !(pend & (1 << bit))) bit++;
    g->io[IF_REG] &= ~(1 << bit);
    push16(g, g->pc);                    /* 2 M-cycles */
    g->pc = 0x40 + bit * 8;
    tick(g, 4);                          /* set-PC M-cycle */
}

/* ---- one instruction (ticks internally; no cycle return) ---- */
static void execute(GB *g) {
    u8 op;
    if (g->halt_bug) { op = bus_read(g, g->pc); g->halt_bug = false; tick(g, 4); }
    else { op = bus_read(g, g->pc++); tick(g, 4); }   /* opcode fetch M-cycle */

    /* LD r,r' block (0x40-0x7F), 0x76 = HALT */
    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) {
            if (!g->ime && (g->ie & g->io[IF_REG] & 0x1F)) g->halt_bug = true;
            else g->halted = true;
            return;
        }
        reg_set(g, (op >> 3) & 7, reg_get(g, op & 7));
        return;
    }
    if (op >= 0x80 && op <= 0xBF) {
        u8 v = reg_get(g, op & 7);
        switch ((op >> 3) & 7) {
            case 0: alu_add(g, v); break; case 1: alu_adc(g, v); break;
            case 2: alu_sub(g, v); break; case 3: alu_sbc(g, v); break;
            case 4: alu_and(g, v); break; case 5: alu_xor(g, v); break;
            case 6: alu_or(g, v); break;  case 7: alu_cp(g, v); break;
        }
        return;
    }

    switch (op) {
        case 0x00: return;                                 /* NOP */
        case 0x10:                                          /* STOP */
            if (g->cgb && (g->key1 & 1)) {                   /* armed double-speed switch */
                g->double_speed = !g->double_speed;
                g->key1 = g->double_speed ? 0x80 : 0x00;     /* bit7=speed, prepare cleared */
            }
            imm8(g);                                         /* STOP is a 2-byte opcode */
            return;

        case 0x01: wBC(g, imm16(g)); return;
        case 0x11: wDE(g, imm16(g)); return;
        case 0x21: wHL(g, imm16(g)); return;
        case 0x31: g->sp = imm16(g); return;

        case 0x02: wr(g, rBC(g), g->a); return;
        case 0x12: wr(g, rDE(g), g->a); return;
        case 0x22: wr(g, rHL(g), g->a); wHL(g, rHL(g) + 1); return;
        case 0x32: wr(g, rHL(g), g->a); wHL(g, rHL(g) - 1); return;
        case 0x0A: g->a = rd(g, rBC(g)); return;
        case 0x1A: g->a = rd(g, rDE(g)); return;
        case 0x2A: g->a = rd(g, rHL(g)); wHL(g, rHL(g) + 1); return;
        case 0x3A: g->a = rd(g, rHL(g)); wHL(g, rHL(g) - 1); return;

        case 0x03: wBC(g, rBC(g) + 1); tick(g, 4); return;
        case 0x13: wDE(g, rDE(g) + 1); tick(g, 4); return;
        case 0x23: wHL(g, rHL(g) + 1); tick(g, 4); return;
        case 0x33: g->sp++; tick(g, 4); return;
        case 0x0B: wBC(g, rBC(g) - 1); tick(g, 4); return;
        case 0x1B: wDE(g, rDE(g) - 1); tick(g, 4); return;
        case 0x2B: wHL(g, rHL(g) - 1); tick(g, 4); return;
        case 0x3B: g->sp--; tick(g, 4); return;

        case 0x04: g->b = alu_inc(g, g->b); return;
        case 0x0C: g->c = alu_inc(g, g->c); return;
        case 0x14: g->d = alu_inc(g, g->d); return;
        case 0x1C: g->e = alu_inc(g, g->e); return;
        case 0x24: g->h = alu_inc(g, g->h); return;
        case 0x2C: g->l = alu_inc(g, g->l); return;
        case 0x34: { u8 v = alu_inc(g, rd(g, rHL(g))); wr(g, rHL(g), v); return; }
        case 0x3C: g->a = alu_inc(g, g->a); return;
        case 0x05: g->b = alu_dec(g, g->b); return;
        case 0x0D: g->c = alu_dec(g, g->c); return;
        case 0x15: g->d = alu_dec(g, g->d); return;
        case 0x1D: g->e = alu_dec(g, g->e); return;
        case 0x25: g->h = alu_dec(g, g->h); return;
        case 0x2D: g->l = alu_dec(g, g->l); return;
        case 0x35: { u8 v = alu_dec(g, rd(g, rHL(g))); wr(g, rHL(g), v); return; }
        case 0x3D: g->a = alu_dec(g, g->a); return;

        case 0x06: g->b = imm8(g); return;
        case 0x0E: g->c = imm8(g); return;
        case 0x16: g->d = imm8(g); return;
        case 0x1E: g->e = imm8(g); return;
        case 0x26: g->h = imm8(g); return;
        case 0x2E: g->l = imm8(g); return;
        case 0x36: wr(g, rHL(g), imm8(g)); return;
        case 0x3E: g->a = imm8(g); return;

        case 0x07: { u8 c = g->a >> 7; g->a = (g->a << 1) | c;
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return; }
        case 0x0F: { u8 c = g->a & 1; g->a = (g->a >> 1) | (c << 7);
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return; }
        case 0x17: { u8 oc = flag(g, FLAG_C) ? 1 : 0; u8 c = g->a >> 7; g->a = (g->a << 1) | oc;
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return; }
        case 0x1F: { u8 oc = flag(g, FLAG_C) ? 1 : 0; u8 c = g->a & 1; g->a = (g->a >> 1) | (oc << 7);
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return; }

        case 0x27: daa(g); return;
        case 0x2F: g->a = ~g->a; set_flag(g, FLAG_N, 1); set_flag(g, FLAG_H, 1); return;
        case 0x37: set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, 1); return;
        case 0x3F: set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, !flag(g, FLAG_C)); return;

        case 0x09: add_hl(g, rBC(g)); tick(g, 4); return;
        case 0x19: add_hl(g, rDE(g)); tick(g, 4); return;
        case 0x29: add_hl(g, rHL(g)); tick(g, 4); return;
        case 0x39: add_hl(g, g->sp); tick(g, 4); return;

        case 0x18: { i8 e = (i8)imm8(g); tick(g, 4); g->pc += e; return; }
        case 0x20: { i8 e = (i8)imm8(g); if (!flag(g, FLAG_Z)) { tick(g, 4); g->pc += e; } return; }
        case 0x28: { i8 e = (i8)imm8(g); if (flag(g, FLAG_Z))  { tick(g, 4); g->pc += e; } return; }
        case 0x30: { i8 e = (i8)imm8(g); if (!flag(g, FLAG_C)) { tick(g, 4); g->pc += e; } return; }
        case 0x38: { i8 e = (i8)imm8(g); if (flag(g, FLAG_C))  { tick(g, 4); g->pc += e; } return; }

        case 0xC3: { u16 a = imm16(g); tick(g, 4); g->pc = a; return; }
        case 0xC2: { u16 a = imm16(g); if (!flag(g, FLAG_Z)) { tick(g, 4); g->pc = a; } return; }
        case 0xCA: { u16 a = imm16(g); if (flag(g, FLAG_Z))  { tick(g, 4); g->pc = a; } return; }
        case 0xD2: { u16 a = imm16(g); if (!flag(g, FLAG_C)) { tick(g, 4); g->pc = a; } return; }
        case 0xDA: { u16 a = imm16(g); if (flag(g, FLAG_C))  { tick(g, 4); g->pc = a; } return; }
        case 0xE9: g->pc = rHL(g); return;                 /* JP HL: no internal */

        case 0xCD: { u16 a = imm16(g); tick(g, 4); push16(g, g->pc); g->pc = a; return; }
        case 0xC4: { u16 a = imm16(g); if (!flag(g, FLAG_Z)) { tick(g, 4); push16(g, g->pc); g->pc = a; } return; }
        case 0xCC: { u16 a = imm16(g); if (flag(g, FLAG_Z))  { tick(g, 4); push16(g, g->pc); g->pc = a; } return; }
        case 0xD4: { u16 a = imm16(g); if (!flag(g, FLAG_C)) { tick(g, 4); push16(g, g->pc); g->pc = a; } return; }
        case 0xDC: { u16 a = imm16(g); if (flag(g, FLAG_C))  { tick(g, 4); push16(g, g->pc); g->pc = a; } return; }

        case 0xC9: g->pc = pop16(g); tick(g, 4); return;
        case 0xC0: tick(g, 4); if (!flag(g, FLAG_Z)) { u16 a = pop16(g); tick(g, 4); g->pc = a; } return;
        case 0xC8: tick(g, 4); if (flag(g, FLAG_Z))  { u16 a = pop16(g); tick(g, 4); g->pc = a; } return;
        case 0xD0: tick(g, 4); if (!flag(g, FLAG_C)) { u16 a = pop16(g); tick(g, 4); g->pc = a; } return;
        case 0xD8: tick(g, 4); if (flag(g, FLAG_C))  { u16 a = pop16(g); tick(g, 4); g->pc = a; } return;
        case 0xD9: g->pc = pop16(g); g->ime = true; tick(g, 4); return;  /* RETI */

        case 0xC7: tick(g, 4); push16(g, g->pc); g->pc = 0x00; return;
        case 0xCF: tick(g, 4); push16(g, g->pc); g->pc = 0x08; return;
        case 0xD7: tick(g, 4); push16(g, g->pc); g->pc = 0x10; return;
        case 0xDF: tick(g, 4); push16(g, g->pc); g->pc = 0x18; return;
        case 0xE7: tick(g, 4); push16(g, g->pc); g->pc = 0x20; return;
        case 0xEF: tick(g, 4); push16(g, g->pc); g->pc = 0x28; return;
        case 0xF7: tick(g, 4); push16(g, g->pc); g->pc = 0x30; return;
        case 0xFF: tick(g, 4); push16(g, g->pc); g->pc = 0x38; return;

        case 0xC5: tick(g, 4); push16(g, rBC(g)); return;
        case 0xD5: tick(g, 4); push16(g, rDE(g)); return;
        case 0xE5: tick(g, 4); push16(g, rHL(g)); return;
        case 0xF5: tick(g, 4); push16(g, rAF(g)); return;
        case 0xC1: wBC(g, pop16(g)); return;
        case 0xD1: wDE(g, pop16(g)); return;
        case 0xE1: wHL(g, pop16(g)); return;
        case 0xF1: wAF(g, pop16(g)); return;

        case 0xC6: alu_add(g, imm8(g)); return;
        case 0xCE: alu_adc(g, imm8(g)); return;
        case 0xD6: alu_sub(g, imm8(g)); return;
        case 0xDE: alu_sbc(g, imm8(g)); return;
        case 0xE6: alu_and(g, imm8(g)); return;
        case 0xEE: alu_xor(g, imm8(g)); return;
        case 0xF6: alu_or(g, imm8(g)); return;
        case 0xFE: alu_cp(g, imm8(g)); return;

        case 0xE0: wr(g, 0xFF00 + imm8(g), g->a); return;
        case 0xF0: g->a = rd(g, 0xFF00 + imm8(g)); return;
        case 0xE2: wr(g, 0xFF00 + g->c, g->a); return;
        case 0xF2: g->a = rd(g, 0xFF00 + g->c); return;
        case 0xEA: wr(g, imm16(g), g->a); return;
        case 0xFA: g->a = rd(g, imm16(g)); return;

        case 0x08: { u16 a = imm16(g); wr(g, a, g->sp & 0xFF); wr(g, a + 1, g->sp >> 8); return; }
        case 0xF8: { i8 e = (i8)imm8(g); wHL(g, add_sp_e(g, e)); tick(g, 4); return; }
        case 0xF9: g->sp = rHL(g); tick(g, 4); return;
        case 0xE8: { i8 e = (i8)imm8(g); g->sp = add_sp_e(g, e); tick(g, 4); tick(g, 4); return; }

        case 0xF3: g->ime = false; g->ime_pending = false; return;
        case 0xFB: g->ime_pending = true; return;

        case 0xCB: do_cb(g); return;

        default: g->locked = true; return;                 /* undefined op hangs the CPU */
    }
}

int cpu_step(GB *g) {
    u64 start = g->cycles;

    if (g->locked) {                  /* hard hang: clock runs, CPU never resumes */
        tick(g, 4);
        return (int)(g->cycles - start);
    }

    if (g->halted) {
        if (g->ie & g->io[IF_REG] & 0x1F) {
            g->halted = false;
        } else {
            tick(g, 4);
            return (int)(g->cycles - start);
        }
    }

    if (g->ime && (g->ie & g->io[IF_REG] & 0x1F)) {
        service_interrupt(g);
        return (int)(g->cycles - start);
    }

    bool ei_was_pending = g->ime_pending;
    execute(g);
    if (ei_was_pending) { g->ime = true; g->ime_pending = false; }
    return (int)(g->cycles - start);
}

void cpu_init_postboot(GB *g) {
    if (g->cgb) {                      /* CGB boot ROM hands off A=0x11 (CGB id) */
        g->a = 0x11; g->f = 0x80;
        g->b = 0x00; g->c = 0x00;
        g->d = 0x00; g->e = 0x08;
        g->h = 0x00; g->l = 0x7C;
    } else {
        g->a = 0x01; g->f = 0xB0;
        g->b = 0x00; g->c = 0x13;
        g->d = 0x00; g->e = 0xD8;
        g->h = 0x01; g->l = 0x4D;
    }
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    g->ime = false; g->ime_pending = false;
    g->halted = false; g->halt_bug = false; g->locked = false; g->double_speed = false;

    g->div_counter = 0xABCC;
    g->tima = 0; g->tma = 0; g->tac = 0;
    g->io[IF_REG] = 0x01;

    g->lcdc = 0x91; g->stat = 0; g->ly = 0; g->lyc = 0;
    g->scy = 0; g->scx = 0; g->wy = 0; g->wx = 0;
    g->bgp = 0xFC; g->obp0 = 0xFF; g->obp1 = 0xFF;
    g->opri = 0;                       /* CGB boot default: OBJ priority by OAM index */
    g->ppu_dot = 0; g->mode = 2; g->win_line = 0;
    g->dma_pos = 160; g->dma_running = false; g->dma_start = 0;
    g->io[0x00] = 0x00;            /* JOYP post-boot reads 0xCF */
    g->sc = 0x00; g->sb = 0x00;
    g->ie = 0x00;
    apu_init(g);
}
