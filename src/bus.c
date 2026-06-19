/* bus.c - the system memory map and I/O register dispatch. */
#include "gb.h"

#define IF_REG 0x0F   /* index into gb->io for 0xFF0F (interrupt flags) */

/* Read-OR masks for I/O registers handled by the default path: unmapped
 * registers read as 0xFF, and sound registers force their unused bits to 1.
 * (JOYP/SC/TAC/IF/STAT apply their own masks in their dedicated handlers.) */
static const u8 HWIO_OR[0x80] = {
    [0x00 ... 0x7F] = 0xFF,                    /* unmapped -> all 1s */
    [0x10] = 0x80, [0x11] = 0x3F, [0x12] = 0x00, [0x13] = 0xFF, [0x14] = 0xBF,
    [0x16] = 0x3F, [0x17] = 0x00, [0x18] = 0xFF, [0x19] = 0xBF,
    [0x1A] = 0x7F, [0x1B] = 0xFF, [0x1C] = 0x9F, [0x1D] = 0xFF, [0x1E] = 0xBF,
    [0x20] = 0xFF, [0x21] = 0x00, [0x22] = 0x00, [0x23] = 0xBF,
    [0x24] = 0x00, [0x25] = 0x00, [0x26] = 0x70,
    [0x30 ... 0x3F] = 0x00,                    /* wave RAM fully readable */
    [0x46] = 0x00,                             /* DMA reg reads back its value */
};

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

/* WRAM offset for a 0xC000..0xDFFF address: bank 0 is fixed at 0xCxxx, the
 * 0xDxxx region is banked via SVBK on CGB (banks 1-7; 0 reads as 1). */
static inline int wram_off(GB *gb, u16 a) {
    if (a < 0xD000) return a - 0xC000;
    int bank = gb->cgb ? ((gb->svbk & 7) ? (gb->svbk & 7) : 1) : 1;
    return bank * 0x1000 + (a - 0xD000);
}

/* Copy one 0x10-byte block from main memory to VRAM (current bank). */
static void hdma_copy_block(GB *gb) {
    int base = (gb->vbk & 1) * 0x2000;
    for (int i = 0; i < 0x10; i++)
        gb->vram[base + ((gb->hdma_dst + i) & 0x1FFF)] = bus_read(gb, gb->hdma_src + i);
    gb->hdma_src += 0x10;
    gb->hdma_dst += 0x10;
}

/* FF55 write: start a VRAM DMA. Bit 7 = mode (0 general / 1 HBlank); bits 0-6 =
 * block count - 1. General-purpose copies everything at once; HBlank mode copies
 * one block per HBlank (stepped from the PPU). Writing bit7=0 mid-HBlank stops it. */
static void hdma_trigger(GB *gb, u8 val) {
    u16 src = (gb->io[0x51] << 8) | (gb->io[0x52] & 0xF0);
    u16 dst = 0x8000 | ((gb->io[0x53] & 0x1F) << 8) | (gb->io[0x54] & 0xF0);
    int blocks = (val & 0x7F) + 1;
    if (!(val & 0x80)) {                       /* general-purpose (or stop) */
        if (gb->hdma_active) { gb->hdma_active = false; gb->hdma_len |= 0x80; return; }
        gb->hdma_src = src; gb->hdma_dst = dst;
        for (int b = 0; b < blocks; b++) hdma_copy_block(gb);
        gb->hdma_len = 0xFF;                    /* done */
    } else {                                   /* HBlank-driven */
        gb->hdma_src = src; gb->hdma_dst = dst;
        gb->hdma_len = val & 0x7F;
        gb->hdma_active = true;
    }
}

/* Step an HBlank-mode transfer: one block per HBlank until exhausted. */
void hdma_hblank_step(GB *gb) {
    if (!gb->hdma_active) return;
    hdma_copy_block(gb);
    if (gb->hdma_len == 0) { gb->hdma_active = false; gb->hdma_len = 0xFF; }
    else gb->hdma_len--;
}

u8 bus_read(GB *gb, u16 addr) {
    if (addr < 0x8000) return cart_read(gb, addr);
    if (addr < 0xA000) return ppu_vram_accessible(gb)
        ? gb->vram[(gb->vbk & 1) * 0x2000 + (addr - 0x8000)] : 0xFF;
    if (addr < 0xC000) return cart_read(gb, addr);
    if (addr < 0xE000) return gb->wram[wram_off(gb, addr)];
    if (addr < 0xFE00) return gb->wram[wram_off(gb, addr - 0x2000)];   /* echo RAM */
    if (addr < 0xFEA0)
        return (gb->dma_running || !ppu_oam_accessible(gb)) ? 0xFF : gb->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;                       /* unusable */
    if (addr < 0xFF80) {                                  /* I/O */
        if ((addr >= 0xFF10 && addr <= 0xFF26) || (addr >= 0xFF30 && addr <= 0xFF3F))
            return apu_read(gb, addr);
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
            case 0xFF4D: return gb->cgb ? (gb->key1 | 0x7E) : 0xFF;      /* KEY1 */
            case 0xFF4F: return gb->cgb ? (gb->vbk | 0xFE) : 0xFF;       /* VBK */
            case 0xFF51: case 0xFF52: case 0xFF53: case 0xFF54:
                return 0xFF;                                             /* HDMA1-4 write-only */
            case 0xFF55: return gb->cgb ? (gb->hdma_active ? (gb->hdma_len & 0x7F)
                                                           : 0xFF) : 0xFF; /* HDMA5 */
            case 0xFF70: return gb->cgb ? (gb->svbk | 0xF8) : 0xFF;      /* SVBK */
            case 0xFF68: return gb->cgb ? gb->bcps : 0xFF;               /* BCPS */
            case 0xFF69: return gb->cgb ? gb->bgpal[gb->bcps & 0x3F] : 0xFF;
            case 0xFF6A: return gb->cgb ? gb->ocps : 0xFF;               /* OCPS */
            case 0xFF6B: return gb->cgb ? gb->objpal[gb->ocps & 0x3F] : 0xFF;
            default:
                return gb->io[addr - 0xFF00] | HWIO_OR[addr - 0xFF00];
        }
    }
    if (addr < 0xFFFF) return gb->hram[addr - 0xFF80];
    return gb->ie;                                         /* 0xFFFF */
}

void bus_write(GB *gb, u16 addr, u8 val) {
    if (addr < 0x8000) { cart_write(gb, addr, val); return; }
    if (addr < 0xA000) {
        if (ppu_vram_accessible(gb)) gb->vram[(gb->vbk & 1) * 0x2000 + (addr - 0x8000)] = val;
        return;
    }
    if (addr < 0xC000) { cart_write(gb, addr, val); return; }
    if (addr < 0xE000) { gb->wram[wram_off(gb, addr)] = val; return; }
    if (addr < 0xFE00) { gb->wram[wram_off(gb, addr - 0x2000)] = val; return; }  /* echo */
    if (addr < 0xFEA0) {
        if (!gb->dma_running && ppu_oam_accessible(gb)) gb->oam[addr - 0xFE00] = val;
        return;
    }
    if (addr < 0xFF00) { return; }                                 /* unusable */
    if (addr < 0xFF80) {                                           /* I/O */
        if ((addr >= 0xFF10 && addr <= 0xFF26) || (addr >= 0xFF30 && addr <= 0xFF3F)) {
            apu_write(gb, addr, val); return;
        }
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
            case 0xFF4D: if (gb->cgb) gb->key1 = (gb->key1 & 0x80) | (val & 1); return; /* KEY1 */
            case 0xFF4F: if (gb->cgb) gb->vbk = val & 1; return;          /* VBK */
            case 0xFF51: case 0xFF52: case 0xFF53: case 0xFF54:
                gb->io[addr - 0xFF00] = val; return;                     /* HDMA1-4 latches */
            case 0xFF55: if (gb->cgb) hdma_trigger(gb, val); return;      /* HDMA5 */
            case 0xFF70: if (gb->cgb) gb->svbk = val & 7; return;         /* SVBK */
            case 0xFF68: if (gb->cgb) gb->bcps = val; return;             /* BCPS */
            case 0xFF69: if (gb->cgb) {                                   /* BCPD */
                gb->bgpal[gb->bcps & 0x3F] = val;
                if (gb->bcps & 0x80) gb->bcps = 0x80 | ((gb->bcps + 1) & 0x3F);
            } return;
            case 0xFF6A: if (gb->cgb) gb->ocps = val; return;            /* OCPS */
            case 0xFF6B: if (gb->cgb) {                                  /* OCPD */
                gb->objpal[gb->ocps & 0x3F] = val;
                if (gb->ocps & 0x80) gb->ocps = 0x80 | ((gb->ocps + 1) & 0x3F);
            } return;
            default:
                gb->io[addr - 0xFF00] = val; return;
        }
    }
    if (addr < 0xFFFF) { gb->hram[addr - 0xFF80] = val; return; }
    gb->ie = val;                                                  /* 0xFFFF */
}
