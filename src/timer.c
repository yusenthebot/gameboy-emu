/* timer.c - DIV/TIMA/TMA/TAC with falling-edge detection and the
 * 4-cycle TIMA-overflow reload delay. Ticked per T-cycle so the edge
 * logic is correct; the finer write-during-reload quirks (Mooneye
 * timer tests) are a later frontier item.
 */
#include "gb.h"

static const u8 TAC_BIT[4] = {9, 3, 5, 7};

static inline void timer_increment(GB *gb) {
    u16 old = gb->div_counter;
    gb->div_counter = old + 1;

    /* Delayed reload after overflow (4 T-cycles). */
    if (gb->tima_reload_delay) {
        if (--gb->tima_reload_delay == 0) {
            gb->tima = gb->tma;
            gb->tima_overflow = false;
            cpu_request_interrupt(gb, INT_TIMER);
        }
    }

    u8 sel = TAC_BIT[gb->tac & 3];
    u8 enable = (gb->tac >> 2) & 1;
    u8 before = ((old >> sel) & 1) & enable;
    u8 after = ((gb->div_counter >> sel) & 1) & enable;
    if (before && !after) {
        if (gb->tima == 0xFF) {
            gb->tima = 0;
            gb->tima_reload_delay = 4;
            gb->tima_overflow = true;
        } else {
            gb->tima++;
        }
    }
}

void timer_tick(GB *gb, int tcycles) {
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
            /* Writing DIV resets the whole 16-bit counter. A falling edge
             * on the selected bit can occur as it goes to 0. */
            u8 sel = TAC_BIT[gb->tac & 3];
            u8 enable = (gb->tac >> 2) & 1;
            if (enable && ((gb->div_counter >> sel) & 1)) {
                if (gb->tima == 0xFF) {
                    gb->tima = 0;
                    gb->tima_reload_delay = 4;
                    gb->tima_overflow = true;
                } else {
                    gb->tima++;
                }
            }
            gb->div_counter = 0;
            break;
        }
        case 0xFF05:
            /* Writing during the reload-delay window cancels the reload. */
            if (gb->tima_reload_delay) {
                gb->tima_reload_delay = 0;
                gb->tima_overflow = false;
            }
            gb->tima = val;
            break;
        case 0xFF06:
            gb->tma = val;
            break;
        case 0xFF07:
            gb->tac = val & 0x07;
            break;
        default:
            break;
    }
}
