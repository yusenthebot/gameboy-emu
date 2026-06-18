/* timer.c - DIV/TIMA/TMA/TAC.
 *
 * Falling-edge increment on the TAC-selected DIV bit, the 4-T TIMA-overflow
 * reload delay, plus the obscure write quirks Mooneye tests:
 *  - writing TAC (or DIV) can glitch a falling edge -> a TIMA increment
 *  - writing TIMA on the reload cycle is ignored (reload wins)
 *  - writing TMA on the reload cycle makes the reload use the new value
 * tima_reloaded marks "the reload happened during the current M-cycle"; it is
 * cleared at the start of every M-cycle tick and consulted by the FF05/FF06
 * write handlers (which run after that tick, per the tick-before-access model).
 */
#include "gb.h"

static const u8 TAC_BIT[4] = {9, 3, 5, 7};

/* The timer's increment signal: selected DIV bit ANDed with the enable bit. */
static inline u8 timer_signal(GB *gb, u8 tac) {
    u8 sel = TAC_BIT[tac & 3];
    return (u8)(((gb->div_counter >> sel) & 1) & ((tac >> 2) & 1));
}

static inline void tima_step(GB *gb) {
    if (gb->tima == 0xFF) {
        gb->tima = 0;
        gb->tima_reload_delay = 4;
        gb->tima_overflow = true;
    } else {
        gb->tima++;
    }
}

static inline void timer_increment(GB *gb) {
    u16 old = gb->div_counter;
    gb->div_counter = old + 1;

    if (gb->tima_reload_delay) {
        if (--gb->tima_reload_delay == 0) {
            gb->tima = gb->tma;
            gb->tima_overflow = false;
            gb->tima_reloaded = true;
            cpu_request_interrupt(gb, INT_TIMER);
        }
    }

    u8 sel = TAC_BIT[gb->tac & 3];
    u8 enable = (gb->tac >> 2) & 1;
    u8 before = ((old >> sel) & 1) & enable;
    u8 after = ((gb->div_counter >> sel) & 1) & enable;
    if (before && !after) tima_step(gb);
}

void timer_tick(GB *gb, int tcycles) {
    gb->tima_reloaded = false;          /* one M-cycle per call (t = 4) */
    for (int i = 0; i < tcycles; i++)
        timer_increment(gb);
}

u8 timer_read(GB *gb, u16 addr) {
    switch (addr) {
        case 0xFF04: return (u8)(gb->div_counter >> 8);
        case 0xFF05: return gb->tima;
        case 0xFF06: return gb->tma;
        case 0xFF07: return gb->tac | 0xF8; /* unused bits read 1 */
        default:     return 0xFF;
    }
}

void timer_write(GB *gb, u16 addr, u8 val) {
    switch (addr) {
        case 0xFF04: {
            /* Writing DIV resets the counter; the resulting falling edge on
             * the selected bit can glitch a TIMA increment. */
            u8 old_sig = timer_signal(gb, gb->tac);
            gb->div_counter = 0;
            if (old_sig && !timer_signal(gb, gb->tac)) tima_step(gb);
            break;
        }
        case 0xFF05:
            if (gb->tima_reloaded) {
                /* Write on the reload cycle is ignored (reload value wins). */
            } else if (gb->tima_reload_delay) {
                /* Write during the delay cancels the pending reload. */
                gb->tima_reload_delay = 0;
                gb->tima_overflow = false;
                gb->tima = val;
            } else {
                gb->tima = val;
            }
            break;
        case 0xFF06:
            gb->tma = val;
            if (gb->tima_reloaded) gb->tima = val; /* reload uses new TMA */
            break;
        case 0xFF07: {
            u8 old_sig = timer_signal(gb, gb->tac);
            gb->tac = val & 0x07;
            if (old_sig && !timer_signal(gb, gb->tac)) tima_step(gb);
            break;
        }
        default:
            break;
    }
}
