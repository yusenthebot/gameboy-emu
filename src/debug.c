/* debug.c - a small interactive CLI debugger (reads commands from stdin).
 *
 *   r / regs            registers + flags
 *   s / step [n]        execute n instructions (default 1)
 *   b / break <addr>    add a PC breakpoint
 *   c / continue        run until a breakpoint (or a safety cycle cap)
 *   m / mem <addr> [n]  hex-dump n bytes (default 16)
 *   d / disasm [addr][n] disassemble n instructions (default PC, 8)
 *   q / quit            leave the debugger
 *
 * Output is deterministic for a given ROM + command script, so a scripted
 * session is gate-testable.
 */
#include "gb.h"
#include <stdio.h>
#include <string.h>

#define MAX_BP 16

static void print_regs(GB *g) {
    printf("AF=%02X%02X BC=%02X%02X DE=%02X%02X HL=%02X%02X SP=%04X PC=%04X [%c%c%c%c]\n",
           g->a, g->f, g->b, g->c, g->d, g->e, g->h, g->l, g->sp, g->pc,
           (g->f & 0x80) ? 'Z' : '-', (g->f & 0x40) ? 'N' : '-',
           (g->f & 0x20) ? 'H' : '-', (g->f & 0x10) ? 'C' : '-');
}

static void print_disasm(GB *g, u16 addr, int n) {
    char buf[40];
    for (int i = 0; i < n; i++) {
        int len = disasm(g, addr, buf, sizeof buf);
        printf("  %04X: %-18s;", addr, buf);
        for (int b = 0; b < len; b++) printf(" %02X", bus_read(g, (u16)(addr + b)));
        printf("\n");
        addr = (u16)(addr + len);
    }
}

void debugger_repl(GB *g) {
    u16 bp[MAX_BP]; int nbp = 0;
    char line[128];
    printf("gbemu debugger — r(egs) s(tep) b(reak) c(ont) m(em) d(isasm) q(uit)\n");
    print_regs(g);
    print_disasm(g, g->pc, 1);
    while (fgets(line, sizeof line, stdin)) {
        char cmd[16] = {0}; unsigned a1 = 0, a2 = 0;
        int na = sscanf(line, "%15s %x %x", cmd, &a1, &a2);
        if (na < 1) continue;
        if (!strcmp(cmd, "q") || !strcmp(cmd, "quit")) break;
        else if (!strcmp(cmd, "r") || !strcmp(cmd, "regs")) print_regs(g);
        else if (!strcmp(cmd, "s") || !strcmp(cmd, "step")) {
            int n = (na >= 2) ? (int)a1 : 1; if (n < 1) n = 1;
            for (int i = 0; i < n; i++) cpu_step(g);
            print_regs(g); print_disasm(g, g->pc, 1);
        }
        else if (!strcmp(cmd, "b") || !strcmp(cmd, "break")) {
            if (na >= 2 && nbp < MAX_BP) { bp[nbp++] = (u16)a1; printf("breakpoint @ %04X\n", (u16)a1); }
        }
        else if (!strcmp(cmd, "c") || !strcmp(cmd, "cont") || !strcmp(cmd, "continue")) {
            u64 cap = g->cycles + 100000000ULL; bool hit = false;
            while (g->cycles < cap && !hit) {
                cpu_step(g);
                for (int i = 0; i < nbp; i++)
                    if (g->pc == bp[i]) { printf("hit breakpoint @ %04X\n", g->pc); hit = true; break; }
            }
            if (!hit) printf("(cycle cap)\n");
            print_regs(g); print_disasm(g, g->pc, 1);
        }
        else if (!strcmp(cmd, "m") || !strcmp(cmd, "mem")) {
            u16 addr = (u16)a1; int n = (na >= 3) ? (int)a2 : 16;
            for (int i = 0; i < n; i++) {
                if (i % 16 == 0) printf("%04X:", (u16)(addr + i));
                printf(" %02X", bus_read(g, (u16)(addr + i)));
                if (i % 16 == 15) printf("\n");
            }
            if (n % 16) printf("\n");
        }
        else if (!strcmp(cmd, "d") || !strcmp(cmd, "disasm")) {
            u16 addr = (na >= 2) ? (u16)a1 : g->pc; int n = (na >= 3) ? (int)a2 : 8;
            print_disasm(g, addr, n);
        }
        else printf("?\n");
        fflush(stdout);
    }
}
