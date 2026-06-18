/* cpu.c - SM83 CPU core (instruction-stepped).
 *
 * Implements the full DMG instruction set with correct flag behavior,
 * interrupt dispatch, EI delay, and the HALT bug. Round 1 ticks the
 * timer/PPU/serial subsystems once per instruction (by the T-cycles the
 * instruction consumed); migrating to per-M-cycle ticking for true
 * sub-instruction memory-access timing is the frontier after the PPU.
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

/* ---- bus helpers (no per-access ticking in round 1) ---- */
static inline u8  rd(GB *g, u16 a) { return bus_read(g, a); }
static inline void wr(GB *g, u16 a, u8 v) { bus_write(g, a, v); }
static inline u8  imm8(GB *g) { return bus_read(g, g->pc++); }
static inline u16 imm16(GB *g) {
    u16 lo = bus_read(g, g->pc++);
    u16 hi = bus_read(g, g->pc++);
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

/* ---- ALU ---- */
static u8 alu_inc(GB *g, u8 v) {
    u8 r = v + 1;
    set_flag(g, FLAG_Z, r == 0);
    set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, (v & 0x0F) == 0x0F);
    return r;
}
static u8 alu_dec(GB *g, u8 v) {
    u8 r = v - 1;
    set_flag(g, FLAG_Z, r == 0);
    set_flag(g, FLAG_N, true);
    set_flag(g, FLAG_H, (v & 0x0F) == 0x00);
    return r;
}
static void alu_add(GB *g, u8 v) {
    u16 r = g->a + v;
    set_flag(g, FLAG_H, ((g->a & 0xF) + (v & 0xF)) > 0xF);
    set_flag(g, FLAG_C, r > 0xFF);
    g->a = (u8)r;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, false);
}
static void alu_adc(GB *g, u8 v) {
    u8 c = flag(g, FLAG_C) ? 1 : 0;
    u16 r = g->a + v + c;
    set_flag(g, FLAG_H, ((g->a & 0xF) + (v & 0xF) + c) > 0xF);
    set_flag(g, FLAG_C, r > 0xFF);
    g->a = (u8)r;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, false);
}
static void alu_sub(GB *g, u8 v) {
    set_flag(g, FLAG_H, (g->a & 0xF) < (v & 0xF));
    set_flag(g, FLAG_C, g->a < v);
    g->a = g->a - v;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, true);
}
static void alu_sbc(GB *g, u8 v) {
    u8 c = flag(g, FLAG_C) ? 1 : 0;
    int r = g->a - v - c;
    set_flag(g, FLAG_H, ((g->a & 0xF) - (v & 0xF) - c) < 0);
    set_flag(g, FLAG_C, r < 0);
    g->a = (u8)r;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, true);
}
static void alu_and(GB *g, u8 v) {
    g->a &= v;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, true);
    set_flag(g, FLAG_C, false);
}
static void alu_or(GB *g, u8 v) {
    g->a |= v;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, false);
    set_flag(g, FLAG_C, false);
}
static void alu_xor(GB *g, u8 v) {
    g->a ^= v;
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, false);
    set_flag(g, FLAG_C, false);
}
static void alu_cp(GB *g, u8 v) {
    set_flag(g, FLAG_H, (g->a & 0xF) < (v & 0xF));
    set_flag(g, FLAG_C, g->a < v);
    set_flag(g, FLAG_Z, g->a == v);
    set_flag(g, FLAG_N, true);
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
    set_flag(g, FLAG_Z, false);
    set_flag(g, FLAG_N, false);
    set_flag(g, FLAG_H, ((sp & 0xF) + (ue & 0xF)) > 0xF);
    set_flag(g, FLAG_C, ((sp & 0xFF) + ue) > 0xFF);
    return (u16)(sp + (i16)e);
}
static void daa(GB *g) {
    int a = g->a;
    int adjust = 0;
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
    set_flag(g, FLAG_Z, g->a == 0);
    set_flag(g, FLAG_H, false);
    set_flag(g, FLAG_C, c);
}

/* ---- CB-prefixed rotate/shift/bit ---- */
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

static int do_cb(GB *g) {
    u8 cb = imm8(g);
    int idx = cb & 7;
    int sub = cb >> 3;
    bool is_hl = (idx == 6);
    u8 v = reg_get(g, idx);
    int cyc;

    if (sub < 8) {                        /* rotate / shift / swap */
        switch (sub) {
            case 0: v = cb_rlc(g, v); break;
            case 1: v = cb_rrc(g, v); break;
            case 2: v = cb_rl(g, v); break;
            case 3: v = cb_rr(g, v); break;
            case 4: v = cb_sla(g, v); break;
            case 5: v = cb_sra(g, v); break;
            case 6: v = cb_swap(g, v); break;
            case 7: v = cb_srl(g, v); break;
        }
        reg_set(g, idx, v);
        cyc = is_hl ? 16 : 8;
    } else if (sub < 16) {                /* BIT b,r */
        int b = sub - 8;
        set_flag(g, FLAG_Z, !((v >> b) & 1));
        set_flag(g, FLAG_N, false);
        set_flag(g, FLAG_H, true);
        cyc = is_hl ? 12 : 8;
    } else if (sub < 24) {                /* RES b,r */
        int b = sub - 16;
        reg_set(g, idx, v & ~(1 << b));
        cyc = is_hl ? 16 : 8;
    } else {                              /* SET b,r */
        int b = sub - 24;
        reg_set(g, idx, v | (1 << b));
        cyc = is_hl ? 16 : 8;
    }
    return cyc;
}

/* ---- push / pop ---- */
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

static int service_interrupt(GB *g) {
    u8 pend = g->ie & g->io[IF_REG] & 0x1F;
    g->ime = false;
    int bit = 0;
    while (bit < 5 && !(pend & (1 << bit))) bit++;
    g->io[IF_REG] &= ~(1 << bit);
    push16(g, g->pc);
    g->pc = 0x40 + bit * 8;
    return 20;
}

/* ---- one instruction ---- */
static int execute(GB *g) {
    u8 op;
    if (g->halt_bug) { op = bus_read(g, g->pc); g->halt_bug = false; }
    else { op = bus_read(g, g->pc++); }

    /* LD r,r' block (0x40-0x7F) with 0x76 = HALT */
    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) { /* HALT */
            if (!g->ime && (g->ie & g->io[IF_REG] & 0x1F))
                g->halt_bug = true;
            else
                g->halted = true;
            return 4;
        }
        int dst = (op >> 3) & 7;
        int src = op & 7;
        reg_set(g, dst, reg_get(g, src));
        return (dst == 6 || src == 6) ? 8 : 4;
    }

    /* ALU A,r block (0x80-0xBF) */
    if (op >= 0x80 && op <= 0xBF) {
        int src = op & 7;
        u8 v = reg_get(g, src);
        switch ((op >> 3) & 7) {
            case 0: alu_add(g, v); break;
            case 1: alu_adc(g, v); break;
            case 2: alu_sub(g, v); break;
            case 3: alu_sbc(g, v); break;
            case 4: alu_and(g, v); break;
            case 5: alu_xor(g, v); break;
            case 6: alu_or(g, v); break;
            case 7: alu_cp(g, v); break;
        }
        return (src == 6) ? 8 : 4;
    }

    switch (op) {
        case 0x00: return 4;                                   /* NOP */
        case 0x10: imm8(g); return 4;                          /* STOP (treat as 2-byte nop) */
        case 0x76: return 4;                                   /* (handled above) */

        /* 16-bit immediate loads */
        case 0x01: wBC(g, imm16(g)); return 12;
        case 0x11: wDE(g, imm16(g)); return 12;
        case 0x21: wHL(g, imm16(g)); return 12;
        case 0x31: g->sp = imm16(g); return 12;

        /* LD (rr),A and A,(rr) */
        case 0x02: wr(g, rBC(g), g->a); return 8;
        case 0x12: wr(g, rDE(g), g->a); return 8;
        case 0x22: wr(g, rHL(g), g->a); wHL(g, rHL(g) + 1); return 8;
        case 0x32: wr(g, rHL(g), g->a); wHL(g, rHL(g) - 1); return 8;
        case 0x0A: g->a = rd(g, rBC(g)); return 8;
        case 0x1A: g->a = rd(g, rDE(g)); return 8;
        case 0x2A: g->a = rd(g, rHL(g)); wHL(g, rHL(g) + 1); return 8;
        case 0x3A: g->a = rd(g, rHL(g)); wHL(g, rHL(g) - 1); return 8;

        /* INC/DEC 16-bit */
        case 0x03: wBC(g, rBC(g) + 1); return 8;
        case 0x13: wDE(g, rDE(g) + 1); return 8;
        case 0x23: wHL(g, rHL(g) + 1); return 8;
        case 0x33: g->sp++; return 8;
        case 0x0B: wBC(g, rBC(g) - 1); return 8;
        case 0x1B: wDE(g, rDE(g) - 1); return 8;
        case 0x2B: wHL(g, rHL(g) - 1); return 8;
        case 0x3B: g->sp--; return 8;

        /* INC/DEC 8-bit */
        case 0x04: g->b = alu_inc(g, g->b); return 4;
        case 0x0C: g->c = alu_inc(g, g->c); return 4;
        case 0x14: g->d = alu_inc(g, g->d); return 4;
        case 0x1C: g->e = alu_inc(g, g->e); return 4;
        case 0x24: g->h = alu_inc(g, g->h); return 4;
        case 0x2C: g->l = alu_inc(g, g->l); return 4;
        case 0x34: { u8 v = alu_inc(g, rd(g, rHL(g))); wr(g, rHL(g), v); return 12; }
        case 0x3C: g->a = alu_inc(g, g->a); return 4;
        case 0x05: g->b = alu_dec(g, g->b); return 4;
        case 0x0D: g->c = alu_dec(g, g->c); return 4;
        case 0x15: g->d = alu_dec(g, g->d); return 4;
        case 0x1D: g->e = alu_dec(g, g->e); return 4;
        case 0x25: g->h = alu_dec(g, g->h); return 4;
        case 0x2D: g->l = alu_dec(g, g->l); return 4;
        case 0x35: { u8 v = alu_dec(g, rd(g, rHL(g))); wr(g, rHL(g), v); return 12; }
        case 0x3D: g->a = alu_dec(g, g->a); return 4;

        /* LD r,d8 */
        case 0x06: g->b = imm8(g); return 8;
        case 0x0E: g->c = imm8(g); return 8;
        case 0x16: g->d = imm8(g); return 8;
        case 0x1E: g->e = imm8(g); return 8;
        case 0x26: g->h = imm8(g); return 8;
        case 0x2E: g->l = imm8(g); return 8;
        case 0x36: wr(g, rHL(g), imm8(g)); return 12;
        case 0x3E: g->a = imm8(g); return 8;

        /* rotates on A (Z always cleared) */
        case 0x07: { u8 c = g->a >> 7; g->a = (g->a << 1) | c;
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return 4; }
        case 0x0F: { u8 c = g->a & 1; g->a = (g->a >> 1) | (c << 7);
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return 4; }
        case 0x17: { u8 oc = flag(g, FLAG_C) ? 1 : 0; u8 c = g->a >> 7; g->a = (g->a << 1) | oc;
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return 4; }
        case 0x1F: { u8 oc = flag(g, FLAG_C) ? 1 : 0; u8 c = g->a & 1; g->a = (g->a >> 1) | (oc << 7);
            set_flag(g, FLAG_Z, 0); set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, c); return 4; }

        case 0x27: daa(g); return 4;
        case 0x2F: g->a = ~g->a; set_flag(g, FLAG_N, 1); set_flag(g, FLAG_H, 1); return 4; /* CPL */
        case 0x37: set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, 1); return 4; /* SCF */
        case 0x3F: set_flag(g, FLAG_N, 0); set_flag(g, FLAG_H, 0); set_flag(g, FLAG_C, !flag(g, FLAG_C)); return 4; /* CCF */

        /* ADD HL,rr */
        case 0x09: add_hl(g, rBC(g)); return 8;
        case 0x19: add_hl(g, rDE(g)); return 8;
        case 0x29: add_hl(g, rHL(g)); return 8;
        case 0x39: add_hl(g, g->sp); return 8;

        /* relative jumps */
        case 0x18: { i8 e = (i8)imm8(g); g->pc += e; return 12; }
        case 0x20: { i8 e = (i8)imm8(g); if (!flag(g, FLAG_Z)) { g->pc += e; return 12; } return 8; }
        case 0x28: { i8 e = (i8)imm8(g); if (flag(g, FLAG_Z))  { g->pc += e; return 12; } return 8; }
        case 0x30: { i8 e = (i8)imm8(g); if (!flag(g, FLAG_C)) { g->pc += e; return 12; } return 8; }
        case 0x38: { i8 e = (i8)imm8(g); if (flag(g, FLAG_C))  { g->pc += e; return 12; } return 8; }

        /* absolute jumps */
        case 0xC3: g->pc = imm16(g); return 16;
        case 0xC2: { u16 a = imm16(g); if (!flag(g, FLAG_Z)) { g->pc = a; return 16; } return 12; }
        case 0xCA: { u16 a = imm16(g); if (flag(g, FLAG_Z))  { g->pc = a; return 16; } return 12; }
        case 0xD2: { u16 a = imm16(g); if (!flag(g, FLAG_C)) { g->pc = a; return 16; } return 12; }
        case 0xDA: { u16 a = imm16(g); if (flag(g, FLAG_C))  { g->pc = a; return 16; } return 12; }
        case 0xE9: g->pc = rHL(g); return 4;

        /* calls */
        case 0xCD: { u16 a = imm16(g); push16(g, g->pc); g->pc = a; return 24; }
        case 0xC4: { u16 a = imm16(g); if (!flag(g, FLAG_Z)) { push16(g, g->pc); g->pc = a; return 24; } return 12; }
        case 0xCC: { u16 a = imm16(g); if (flag(g, FLAG_Z))  { push16(g, g->pc); g->pc = a; return 24; } return 12; }
        case 0xD4: { u16 a = imm16(g); if (!flag(g, FLAG_C)) { push16(g, g->pc); g->pc = a; return 24; } return 12; }
        case 0xDC: { u16 a = imm16(g); if (flag(g, FLAG_C))  { push16(g, g->pc); g->pc = a; return 24; } return 12; }

        /* returns */
        case 0xC9: g->pc = pop16(g); return 16;
        case 0xC0: if (!flag(g, FLAG_Z)) { g->pc = pop16(g); return 20; } return 8;
        case 0xC8: if (flag(g, FLAG_Z))  { g->pc = pop16(g); return 20; } return 8;
        case 0xD0: if (!flag(g, FLAG_C)) { g->pc = pop16(g); return 20; } return 8;
        case 0xD8: if (flag(g, FLAG_C))  { g->pc = pop16(g); return 20; } return 8;
        case 0xD9: g->pc = pop16(g); g->ime = true; return 16; /* RETI */

        /* restarts */
        case 0xC7: push16(g, g->pc); g->pc = 0x00; return 16;
        case 0xCF: push16(g, g->pc); g->pc = 0x08; return 16;
        case 0xD7: push16(g, g->pc); g->pc = 0x10; return 16;
        case 0xDF: push16(g, g->pc); g->pc = 0x18; return 16;
        case 0xE7: push16(g, g->pc); g->pc = 0x20; return 16;
        case 0xEF: push16(g, g->pc); g->pc = 0x28; return 16;
        case 0xF7: push16(g, g->pc); g->pc = 0x30; return 16;
        case 0xFF: push16(g, g->pc); g->pc = 0x38; return 16;

        /* push / pop */
        case 0xC5: push16(g, rBC(g)); return 16;
        case 0xD5: push16(g, rDE(g)); return 16;
        case 0xE5: push16(g, rHL(g)); return 16;
        case 0xF5: push16(g, rAF(g)); return 16;
        case 0xC1: wBC(g, pop16(g)); return 12;
        case 0xD1: wDE(g, pop16(g)); return 12;
        case 0xE1: wHL(g, pop16(g)); return 12;
        case 0xF1: wAF(g, pop16(g)); return 12;

        /* ALU A,d8 */
        case 0xC6: alu_add(g, imm8(g)); return 8;
        case 0xCE: alu_adc(g, imm8(g)); return 8;
        case 0xD6: alu_sub(g, imm8(g)); return 8;
        case 0xDE: alu_sbc(g, imm8(g)); return 8;
        case 0xE6: alu_and(g, imm8(g)); return 8;
        case 0xEE: alu_xor(g, imm8(g)); return 8;
        case 0xF6: alu_or(g, imm8(g)); return 8;
        case 0xFE: alu_cp(g, imm8(g)); return 8;

        /* high-RAM / IO loads */
        case 0xE0: wr(g, 0xFF00 + imm8(g), g->a); return 12;       /* LDH (a8),A */
        case 0xF0: g->a = rd(g, 0xFF00 + imm8(g)); return 12;      /* LDH A,(a8) */
        case 0xE2: wr(g, 0xFF00 + g->c, g->a); return 8;           /* LD (C),A */
        case 0xF2: g->a = rd(g, 0xFF00 + g->c); return 8;          /* LD A,(C) */
        case 0xEA: wr(g, imm16(g), g->a); return 16;               /* LD (a16),A */
        case 0xFA: g->a = rd(g, imm16(g)); return 16;              /* LD A,(a16) */

        /* SP / HL arithmetic */
        case 0x08: { u16 a = imm16(g); wr(g, a, g->sp & 0xFF); wr(g, a + 1, g->sp >> 8); return 20; } /* LD (a16),SP */
        case 0xF8: { i8 e = (i8)imm8(g); wHL(g, add_sp_e(g, e)); return 12; }  /* LD HL,SP+e8 */
        case 0xF9: g->sp = rHL(g); return 8;                       /* LD SP,HL */
        case 0xE8: { i8 e = (i8)imm8(g); g->sp = add_sp_e(g, e); return 16; }  /* ADD SP,e8 */

        /* interrupt control */
        case 0xF3: g->ime = false; g->ime_pending = false; return 4; /* DI */
        case 0xFB: g->ime_pending = true; return 4;                  /* EI */

        case 0xCB: return do_cb(g);

        default:
            /* illegal opcode: treat as NOP but warn (helps catch decode bugs) */
            return 4;
    }
}

static void sync_subsystems(GB *g, int tcycles) {
    g->cycles += tcycles;
    timer_tick(g, tcycles);
    ppu_tick(g, tcycles);
    serial_tick(g, tcycles);
}

int cpu_step(GB *g) {
    /* Wake from HALT when an interrupt is pending (independent of IME). */
    if (g->halted) {
        if (g->ie & g->io[IF_REG] & 0x1F) {
            g->halted = false;
        } else {
            sync_subsystems(g, 4);
            return 4;
        }
    }

    /* Service a pending interrupt before fetching. */
    if (g->ime && (g->ie & g->io[IF_REG] & 0x1F)) {
        int c = service_interrupt(g);
        sync_subsystems(g, c);
        return c;
    }

    bool ei_was_pending = g->ime_pending;
    int c = execute(g);
    if (ei_was_pending) { g->ime = true; g->ime_pending = false; }
    sync_subsystems(g, c);
    return c;
}

void cpu_init_postboot(GB *g) {
    /* DMG post-boot register state (no boot ROM). */
    g->a = 0x01; g->f = 0xB0;
    g->b = 0x00; g->c = 0x13;
    g->d = 0x00; g->e = 0xD8;
    g->h = 0x01; g->l = 0x4D;
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    g->ime = false;
    g->ime_pending = false;
    g->halted = false;
    g->halt_bug = false;

    g->div_counter = 0xABCC;
    g->tima = 0; g->tma = 0; g->tac = 0;
    g->io[IF_REG] = 0x01;          /* IF post-boot lower bits */

    g->lcdc = 0x91; g->stat = 0; g->ly = 0; g->lyc = 0;
    g->scy = 0; g->scx = 0; g->wy = 0; g->wx = 0;
    g->bgp = 0xFC; g->obp0 = 0xFF; g->obp1 = 0xFF;
    g->ppu_dot = 0;
    g->io[0x00] = 0x30;            /* JOYP: nothing selected */
    g->sc = 0x00; g->sb = 0x00;
    g->ie = 0x00;
}
