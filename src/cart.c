/* cart.c - cartridge loading and MBC banking.
 *
 * Supports: ROM-only, MBC1 (incl. the 5-bit BANK1 / 2-bit BANK2 / mode
 * register semantics), MBC2 (built-in 512x4-bit RAM, address-bit-8 register
 * select), and MBC5 (9-bit ROM bank, 4-bit RAM bank). MBC3/RTC is a later item.
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int RAM_SIZE_TABLE[] = {0, 0, 8 * 1024, 32 * 1024, 128 * 1024, 64 * 1024};

int cart_load(GB *gb, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cart: cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0x150) {
        fprintf(stderr, "cart: file too small (%ld bytes)\n", sz);
        fclose(f);
        return -1;
    }
    u8 *rom = malloc((size_t)sz);
    if (fread(rom, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "cart: short read\n");
        free(rom);
        fclose(f);
        return -1;
    }
    fclose(f);

    Cart *c = &gb->cart;
    memset(c, 0, sizeof(*c));
    c->rom = rom;
    c->rom_size = (size_t)sz;

    memcpy(c->title, rom + 0x134, 16);
    c->title[16] = 0;

    u8 type = rom[0x147];
    switch (type) {
        case 0x00:                         c->mbc = 0; break;
        case 0x01: case 0x02: case 0x03:   c->mbc = 1; break;
        case 0x05: case 0x06:              c->mbc = 2; break;
        case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E:   c->mbc = 5; break;
        default:
            c->mbc = 1;
            fprintf(stderr, "cart: unhandled MBC type 0x%02X, assuming MBC1\n", type);
            break;
    }
    c->has_battery = (type == 0x03 || type == 0x06 || type == 0x09 ||
                      type == 0x0F || type == 0x10 || type == 0x13 ||
                      type == 0x1B || type == 0x1E);

    u8 rom_size_code = rom[0x148];
    c->rom_banks = 2 << rom_size_code;            /* 32KB << n */
    if ((size_t)c->rom_banks * 0x4000 > c->rom_size)
        c->rom_banks = (int)(c->rom_size / 0x4000);
    if (c->rom_banks < 2) c->rom_banks = 2;

    if (c->mbc == 2) {
        /* MBC2 has 512 x 4-bit built-in RAM (stored one nibble per byte). */
        c->ram_size = 512;
        c->ram = calloc(1, c->ram_size);
        c->ram_banks = 1;
    } else {
        u8 ram_size_code = rom[0x149];
        c->ram_size = (ram_size_code < (int)(sizeof(RAM_SIZE_TABLE) / sizeof(int)))
                          ? (size_t)RAM_SIZE_TABLE[ram_size_code]
                          : 0;
        if (c->ram_size) {
            c->ram = calloc(1, c->ram_size);
            c->ram_banks = (int)(c->ram_size / 0x2000);
            if (c->ram_banks < 1) c->ram_banks = 1;
        }
    }

    c->bank_lo = 1;
    c->bank_hi = 0;
    c->ram_bank = 0;
    c->mode = 0;
    c->ram_enable = false;

    fprintf(stderr, "cart: '%s' type=0x%02X mbc=%d rom=%dKB(%d banks) ram=%zuB\n",
            c->title, type, c->mbc, (int)(c->rom_size / 1024), c->rom_banks, c->ram_size);
    return 0;
}

void cart_free(GB *gb) {
    free(gb->cart.rom);
    free(gb->cart.ram);
    gb->cart.rom = NULL;
    gb->cart.ram = NULL;
}

static inline u8 rom_byte(Cart *c, int bank, u16 addr) {
    bank &= (c->rom_banks - 1);
    u32 off = (u32)bank * 0x4000 + (addr & 0x3FFF);
    return c->rom[off];
}

/* ---- per-MBC effective ROM bank for the 0x4000-0x7FFF window ---- */
static int high_rom_bank(Cart *c) {
    switch (c->mbc) {
        case 1: {
            int b1 = c->bank_lo & 0x1F;
            if (b1 == 0) b1 = 1;                 /* BANK1 of 0 reads as 1 */
            return (c->bank_hi << 5) | b1;
        }
        case 2: {
            int b = c->bank_lo & 0x0F;
            if (b == 0) b = 1;
            return b;
        }
        case 5:
            return (c->bank_hi << 8) | c->bank_lo; /* 9-bit, 0 is valid */
        default:
            return 1;
    }
}

static int low_rom_bank(Cart *c) {
    /* MBC1 mode 1 maps BANK2 into the low window; everyone else uses bank 0. */
    if (c->mbc == 1 && c->mode == 1)
        return (c->bank_hi << 5);
    return 0;
}

static int ram_bank_num(Cart *c) {
    if (c->mbc == 1) return (c->mode == 1) ? c->bank_hi : 0;
    if (c->mbc == 5) return c->ram_bank & 0x0F;
    return 0;
}

u8 cart_read(GB *gb, u16 addr) {
    Cart *c = &gb->cart;
    if (addr < 0x4000) return rom_byte(c, low_rom_bank(c), addr);
    if (addr < 0x8000) return rom_byte(c, high_rom_bank(c), addr);

    if (addr >= 0xA000 && addr < 0xC000) {
        if (c->mbc == 2) {
            if (!c->ram_enable) return 0xFF;
            /* 512 nibbles, echoed every 0x200; upper nibble reads as 1. */
            return 0xF0 | (c->ram[(addr & 0x1FF)] & 0x0F);
        }
        if (!c->ram || !c->ram_enable) return 0xFF;
        int rbank = ram_bank_num(c);
        if (c->ram_banks) rbank &= (c->ram_banks - 1);
        u32 off = (u32)rbank * 0x2000 + (addr & 0x1FFF);
        return (off < c->ram_size) ? c->ram[off] : 0xFF;
    }
    return 0xFF;
}

void cart_write(GB *gb, u16 addr, u8 val) {
    Cart *c = &gb->cart;

    switch (c->mbc) {
        case 1:
            if (addr < 0x2000)      c->ram_enable = (val & 0x0F) == 0x0A;
            else if (addr < 0x4000) c->bank_lo = val & 0x1F;      /* raw; 0->1 at use */
            else if (addr < 0x6000) c->bank_hi = val & 0x03;
            else if (addr < 0x8000) c->mode = val & 0x01;
            else if (addr >= 0xA000 && addr < 0xC000 && c->ram && c->ram_enable) {
                int rbank = ram_bank_num(c);
                if (c->ram_banks) rbank &= (c->ram_banks - 1);
                u32 off = (u32)rbank * 0x2000 + (addr & 0x1FFF);
                if (off < c->ram_size) c->ram[off] = val;
            }
            return;

        case 2:
            /* Registers live in 0x0000-0x3FFF; address bit 8 selects which. */
            if (addr < 0x4000) {
                if (addr & 0x0100) { c->bank_lo = val & 0x0F; }   /* ROMB */
                else               { c->ram_enable = (val & 0x0F) == 0x0A; } /* RAMG */
            } else if (addr >= 0xA000 && addr < 0xC000 && c->ram_enable) {
                c->ram[(addr & 0x1FF)] = val & 0x0F;
            }
            return;

        case 5:
            if (addr < 0x2000)      c->ram_enable = (val & 0x0F) == 0x0A;
            else if (addr < 0x3000) c->bank_lo = val;             /* ROM bank low 8 */
            else if (addr < 0x4000) c->bank_hi = val & 0x01;      /* ROM bank bit 8 */
            else if (addr < 0x6000) c->ram_bank = val & 0x0F;
            else if (addr >= 0xA000 && addr < 0xC000 && c->ram && c->ram_enable) {
                int rbank = ram_bank_num(c);
                if (c->ram_banks) rbank &= (c->ram_banks - 1);
                u32 off = (u32)rbank * 0x2000 + (addr & 0x1FFF);
                if (off < c->ram_size) c->ram[off] = val;
            }
            return;

        default: /* ROM-only */
            if (addr >= 0xA000 && addr < 0xC000 && c->ram) {
                u32 off = addr & 0x1FFF;
                if (off < c->ram_size) c->ram[off] = val;
            }
            return;
    }
}
