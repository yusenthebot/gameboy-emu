/* ppu_lite.c - a free-running scanline timer (no pixel rendering yet).
 *
 * Round 1 only needs LY/STAT/VBlank to advance so that ROMs which wait
 * on vblank or poll LY make progress, and so timer/STAT interrupts fire.
 * The real scanline FIFO PPU (background, window, sprites, mode timing,
 * STAT quirks) is the major frontier after the CPU is solid.
 */
#include "gb.h"

#define DOTS_PER_LINE 456
#define LINES_PER_FRAME 154
#define VBLANK_LINE 144

static void stat_update_line(GB *gb, u8 mode) {
    /* STAT interrupt is the rising edge of the OR of enabled sources. */
    bool line = false;
    if ((gb->stat & 0x08) && mode == 0) line = true;       /* HBlank */
    if ((gb->stat & 0x10) && mode == 1) line = true;       /* VBlank */
    if ((gb->stat & 0x20) && mode == 2) line = true;       /* OAM */
    if ((gb->stat & 0x40) && gb->ly == gb->lyc) line = true; /* LYC=LY */

    static bool prev_line; /* note: single-PPU emulator, fine as file-static */
    if (line && !prev_line)
        cpu_request_interrupt(gb, INT_STAT);
    prev_line = line;
}

static u8 current_mode(GB *gb) {
    if (gb->ly >= VBLANK_LINE) return 1;          /* VBlank */
    u32 dot = gb->ppu_dot % DOTS_PER_LINE;
    if (dot < 80) return 2;                       /* OAM scan */
    if (dot < 80 + 172) return 3;                 /* pixel transfer */
    return 0;                                     /* HBlank */
}

void ppu_tick(GB *gb, int tcycles) {
    if (!(gb->lcdc & 0x80)) {
        /* LCD off: LY reads 0, no progression. */
        gb->ppu_dot = 0;
        gb->ly = 0;
        return;
    }
    for (int i = 0; i < tcycles; i++) {
        gb->ppu_dot++;
        if (gb->ppu_dot >= DOTS_PER_LINE * LINES_PER_FRAME)
            gb->ppu_dot = 0;
        u8 new_ly = (u8)(gb->ppu_dot / DOTS_PER_LINE);
        if (new_ly != gb->ly) {
            gb->ly = new_ly;
            if (gb->ly == VBLANK_LINE)
                cpu_request_interrupt(gb, INT_VBLANK);
        }
        stat_update_line(gb, current_mode(gb));
    }
}

u8 ppu_read(GB *gb, u16 addr) {
    switch (addr) {
        case 0xFF40: return gb->lcdc;
        case 0xFF41: return (gb->stat & 0x78) | (gb->ly == gb->lyc ? 0x04 : 0) |
                            current_mode(gb) | 0x80;
        case 0xFF42: return gb->scy;
        case 0xFF43: return gb->scx;
        case 0xFF44: return gb->ly;
        case 0xFF45: return gb->lyc;
        case 0xFF47: return gb->bgp;
        case 0xFF48: return gb->obp0;
        case 0xFF49: return gb->obp1;
        case 0xFF4A: return gb->wy;
        case 0xFF4B: return gb->wx;
        default:     return 0xFF;
    }
}

void ppu_write(GB *gb, u16 addr, u8 val) {
    switch (addr) {
        case 0xFF40:
            gb->lcdc = val;
            if (!(val & 0x80)) { gb->ly = 0; gb->ppu_dot = 0; }
            break;
        case 0xFF41: gb->stat = (val & 0x78); break; /* keep only enable bits */
        case 0xFF42: gb->scy = val; break;
        case 0xFF43: gb->scx = val; break;
        case 0xFF45: gb->lyc = val; break;
        case 0xFF47: gb->bgp = val; break;
        case 0xFF48: gb->obp0 = val; break;
        case 0xFF49: gb->obp1 = val; break;
        case 0xFF4A: gb->wy = val; break;
        case 0xFF4B: gb->wx = val; break;
        default: break;
    }
}
