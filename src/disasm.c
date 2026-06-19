/* disasm.c - SM83 disassembler (one instruction -> mnemonic).
 *
 * disasm() decodes the instruction at `addr` via the bus, writes the textual
 * mnemonic into `buf`, and returns the instruction length in bytes. Operand
 * markers in the templates: %n = imm8, %w = imm16, %r = relative (shown as the
 * absolute target). The regular LD r,r' / ALU A,r / CB blocks are generated.
 */
#include "gb.h"
#include <stdio.h>
#include <string.h>

static const char *REG[8] = {"B", "C", "D", "E", "H", "L", "(HL)", "A"};
static const char *ALU[8] = {"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
static const char *ROT[8] = {"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SWAP", "SRL"};

/* Templates for the irregular opcodes (0x00-0x3F and 0xC0-0xFF); the LD/ALU
 * blocks 0x40-0xBF are filled in algorithmically and left NULL here. */
static const char *T[256] = {
/*00*/ "NOP","LD BC,%w","LD (BC),A","INC BC","INC B","DEC B","LD B,%n","RLCA",
/*08*/ "LD (%w),SP","ADD HL,BC","LD A,(BC)","DEC BC","INC C","DEC C","LD C,%n","RRCA",
/*10*/ "STOP","LD DE,%w","LD (DE),A","INC DE","INC D","DEC D","LD D,%n","RLA",
/*18*/ "JR %r","ADD HL,DE","LD A,(DE)","DEC DE","INC E","DEC E","LD E,%n","RRA",
/*20*/ "JR NZ,%r","LD HL,%w","LD (HL+),A","INC HL","INC H","DEC H","LD H,%n","DAA",
/*28*/ "JR Z,%r","ADD HL,HL","LD A,(HL+)","DEC HL","INC L","DEC L","LD L,%n","CPL",
/*30*/ "JR NC,%r","LD SP,%w","LD (HL-),A","INC SP","INC (HL)","DEC (HL)","LD (HL),%n","SCF",
/*38*/ "JR C,%r","ADD HL,SP","LD A,(HL-)","DEC SP","INC A","DEC A","LD A,%n","CCF",
[0xC0]="RET NZ",[0xC1]="POP BC",[0xC2]="JP NZ,%w",[0xC3]="JP %w",[0xC4]="CALL NZ,%w",
[0xC5]="PUSH BC",[0xC6]="ADD A,%n",[0xC7]="RST 00H",
[0xC8]="RET Z",[0xC9]="RET",[0xCA]="JP Z,%w",[0xCB]="CB",[0xCC]="CALL Z,%w",
[0xCD]="CALL %w",[0xCE]="ADC A,%n",[0xCF]="RST 08H",
[0xD0]="RET NC",[0xD1]="POP DE",[0xD2]="JP NC,%w",[0xD3]="??",[0xD4]="CALL NC,%w",
[0xD5]="PUSH DE",[0xD6]="SUB %n",[0xD7]="RST 10H",
[0xD8]="RET C",[0xD9]="RETI",[0xDA]="JP C,%w",[0xDB]="??",[0xDC]="CALL C,%w",
[0xDD]="??",[0xDE]="SBC A,%n",[0xDF]="RST 18H",
[0xE0]="LDH (%n),A",[0xE1]="POP HL",[0xE2]="LD (C),A",[0xE3]="??",[0xE4]="??",
[0xE5]="PUSH HL",[0xE6]="AND %n",[0xE7]="RST 20H",
[0xE8]="ADD SP,%r",[0xE9]="JP HL",[0xEA]="LD (%w),A",[0xEB]="??",[0xEC]="??",
[0xED]="??",[0xEE]="XOR %n",[0xEF]="RST 28H",
[0xF0]="LDH A,(%n)",[0xF1]="POP AF",[0xF2]="LD A,(C)",[0xF3]="DI",[0xF4]="??",
[0xF5]="PUSH AF",[0xF6]="OR %n",[0xF7]="RST 30H",
[0xF8]="LD HL,SP%r",[0xF9]="LD SP,HL",[0xFA]="LD A,(%w)",[0xFB]="EI",[0xFC]="??",
[0xFD]="??",[0xFE]="CP %n",[0xFF]="RST 38H",
};

int disasm(GB *g, u16 addr, char *buf, size_t sz) {
    u8 op = bus_read(g, addr);
    char tmp[32];
    const char *t;

    if (op >= 0x40 && op <= 0x7F) {
        if (op == 0x76) { snprintf(buf, sz, "HALT"); return 1; }
        snprintf(buf, sz, "LD %s,%s", REG[(op >> 3) & 7], REG[op & 7]);
        return 1;
    }
    if (op >= 0x80 && op <= 0xBF) {
        snprintf(buf, sz, "%s%s", ALU[(op >> 3) & 7], REG[op & 7]);
        return 1;
    }
    if (op == 0xCB) {
        u8 cb = bus_read(g, (u16)(addr + 1));
        int idx = cb & 7, sub = cb >> 3;
        if (sub < 8) snprintf(buf, sz, "%s %s", ROT[sub], REG[idx]);
        else if (sub < 16) snprintf(buf, sz, "BIT %d,%s", sub - 8, REG[idx]);
        else if (sub < 24) snprintf(buf, sz, "RES %d,%s", sub - 16, REG[idx]);
        else snprintf(buf, sz, "SET %d,%s", sub - 24, REG[idx]);
        return 2;
    }

    t = T[op];
    if (!t) { snprintf(buf, sz, "DB $%02X", op); return 1; }

    /* substitute operand markers */
    const char *p = strstr(t, "%");
    if (!p) { snprintf(buf, sz, "%s", t); return 1; }
    int prefix = (int)(p - t);
    if (p[1] == 'n') {                               /* imm8 */
        u8 n = bus_read(g, (u16)(addr + 1));
        snprintf(tmp, sizeof tmp, "$%02X", n);
        snprintf(buf, sz, "%.*s%s%s", prefix, t, tmp, p + 2);
        return 2;
    }
    if (p[1] == 'w') {                               /* imm16 */
        u16 w = bus_read(g, (u16)(addr + 1)) | (bus_read(g, (u16)(addr + 2)) << 8);
        snprintf(tmp, sizeof tmp, "$%04X", w);
        snprintf(buf, sz, "%.*s%s%s", prefix, t, tmp, p + 2);
        return 3;
    }
    /* %r : relative — show the absolute target (and signed offset for SP) */
    i8 e = (i8)bus_read(g, (u16)(addr + 1));
    if (op == 0xE8 || op == 0xF8)                    /* ADD SP,e / LD HL,SP+e */
        snprintf(tmp, sizeof tmp, "%+d", e);
    else
        snprintf(tmp, sizeof tmp, "$%04X", (u16)(addr + 2 + e));
    snprintf(buf, sz, "%.*s%s%s", prefix, t, tmp, p + 2);
    return 2;
}
