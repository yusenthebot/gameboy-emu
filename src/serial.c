/* serial.c - link-port serial. For the headless test harness we capture
 * each byte the ROM "transmits" (SB at the moment SC bit7+internal-clock
 * is set) into a log, complete the transfer immediately, and raise the
 * serial interrupt. Cycle-accurate bit shifting is a later frontier item.
 */
#include "gb.h"
#include <stdio.h>

static void serial_emit(GB *gb, u8 ch) {
    if (gb->serial_len < sizeof(gb->serial_log) - 1) {
        gb->serial_log[gb->serial_len++] = (char)ch;
        gb->serial_log[gb->serial_len] = 0;
    }
    /* Mirror to stdout live so progress is visible while a ROM runs. */
    fputc(ch, stdout);
    fflush(stdout);
}

void serial_tick(GB *gb, int tcycles) {
    (void)gb;
    (void)tcycles;
}

u8 serial_read(GB *gb, u16 addr) {
    switch (addr) {
        case 0xFF01: return gb->sb;
        case 0xFF02: return gb->sc | 0x7E; /* unused bits read 1 */
        default:     return 0xFF;
    }
}

void serial_write(GB *gb, u16 addr, u8 val) {
    switch (addr) {
        case 0xFF01:
            gb->sb = val;
            break;
        case 0xFF02:
            gb->sc = val;
            if ((val & 0x81) == 0x81) {
                /* Start transfer, internal clock: emit and complete now. */
                serial_emit(gb, gb->sb);
                gb->sb = 0xFF;
                gb->sc &= ~0x80;
                cpu_request_interrupt(gb, INT_SERIAL);
            }
            break;
        default:
            break;
    }
}
