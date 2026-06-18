/* bus.c - the system memory map and I/O register dispatch. */
#include "gb.h"

#define IF_REG 0x0F   /* index into gb->io for 0xFF0F (interrupt flags) */

static u8 joypad_read(GB *gb) {
    u8 sel = gb->io[0x00] & 0x30;
    u8 res = 0xC0 | sel | 0x0F; /* bits 6,7 high; lower nibble defaults high */
    if (!(sel & 0x10)) {        /* P14=0: directions selected */
        if (gb->buttons & 0x01) res &= ~0x01; /* right */
        if (gb->buttons & 0x02) res &= ~0x02; /* left  */
        if (gb->buttons & 0x04) res &= ~0x04; /* up    */
        if (gb->buttons & 0x08) res &= ~0x08; /* down  */
    }
    if (!(sel & 0x20)) {        /* P15=0: action buttons selected */
        if (gb->buttons & 0x10) res &= ~0x01; /* A      */
        if (gb->buttons & 0x20) res &= ~0x02; /* B      */
        if (gb->buttons & 0x40) res &= ~0x04; /* select */
        if (gb->buttons & 0x80) res &= ~0x08; /* start  */
    }
    return res;
}

/* Cycle-accurate OAM DMA: copies one byte per M-cycle for 160 M-cycles,
 * after a short startup delay. While running, OAM is locked to the CPU.
 * Writing FF46 again restarts the transfer (the in-flight one runs through
 * the new startup delay, then the new source takes over). */
void dma_tick(GB *gb, int tcycles) {
    for (int m = 0; m < tcycles / 4; m++) {
        if (gb->dma_start > 0) {
            if (--gb->dma_start == 0) {
                gb->dma_src = gb->dma_pending_src;
                gb->dma_pos = 0;
                gb->dma_running = true;
            }
        }
        if (gb->dma_running && gb->dma_pos < 160) {
            gb->oam[gb->dma_pos] = bus_read(gb, gb->dma_src + gb->dma_pos);
            if (++gb->dma_pos >= 160) gb->dma_running = false;
        }
    }
}

static void dma_write(GB *gb, u8 val) {
    gb->dma_reg = val;
    gb->dma_pending_src = (u16)val << 8;
    gb->dma_start = 3;        /* startup delay before the transfer begins
                               * (calibrated to Mooneye oam_dma_timing) */
}

u8 bus_read(GB *gb, u16 addr) {
    if (addr < 0x8000) return cart_read(gb, addr);
    if (addr < 0xA000) return gb->vram[addr - 0x8000];
    if (addr < 0xC000) return cart_read(gb, addr);
    if (addr < 0xE000) return gb->wram[addr - 0xC000];
    if (addr < 0xFE00) return gb->wram[addr - 0xE000];   /* echo RAM */
    if (addr < 0xFEA0) return gb->dma_running ? 0xFF : gb->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;                       /* unusable */
    if (addr < 0xFF80) {                                  /* I/O */
        switch (addr) {
            case 0xFF00: return joypad_read(gb);
            case 0xFF01: case 0xFF02: return serial_read(gb, addr);
            case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
                return timer_read(gb, addr);
            case 0xFF0F: return gb->io[IF_REG] | 0xE0;
            case 0xFF40: case 0xFF41: case 0xFF42: case 0xFF43:
            case 0xFF44: case 0xFF45: case 0xFF47: case 0xFF48:
            case 0xFF49: case 0xFF4A: case 0xFF4B:
                return ppu_read(gb, addr);
            default:
                return gb->io[addr - 0xFF00];
        }
    }
    if (addr < 0xFFFF) return gb->hram[addr - 0xFF80];
    return gb->ie;                                         /* 0xFFFF */
}

void bus_write(GB *gb, u16 addr, u8 val) {
    if (addr < 0x8000) { cart_write(gb, addr, val); return; }
    if (addr < 0xA000) { gb->vram[addr - 0x8000] = val; return; }
    if (addr < 0xC000) { cart_write(gb, addr, val); return; }
    if (addr < 0xE000) { gb->wram[addr - 0xC000] = val; return; }
    if (addr < 0xFE00) { gb->wram[addr - 0xE000] = val; return; }  /* echo */
    if (addr < 0xFEA0) { if (!gb->dma_running) gb->oam[addr - 0xFE00] = val; return; }
    if (addr < 0xFF00) { return; }                                 /* unusable */
    if (addr < 0xFF80) {                                           /* I/O */
        switch (addr) {
            case 0xFF00: gb->io[0x00] = val & 0x30; return;
            case 0xFF01: case 0xFF02: serial_write(gb, addr, val); return;
            case 0xFF04: case 0xFF05: case 0xFF06: case 0xFF07:
                timer_write(gb, addr, val); return;
            case 0xFF0F: gb->io[IF_REG] = val & 0x1F; return;
            case 0xFF46: gb->io[0x46] = val; dma_write(gb, val); return;
            case 0xFF40: case 0xFF41: case 0xFF42: case 0xFF43:
            case 0xFF45: case 0xFF47: case 0xFF48: case 0xFF49:
            case 0xFF4A: case 0xFF4B:
                ppu_write(gb, addr, val); return;
            case 0xFF44: return; /* LY is read-only */
            default:
                gb->io[addr - 0xFF00] = val; return;
        }
    }
    if (addr < 0xFFFF) { gb->hram[addr - 0xFF80] = val; return; }
    gb->ie = val;                                                  /* 0xFFFF */
}
