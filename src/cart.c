/* cart.c - cartridge loading and MBC banking.
 * Round 1 supports ROM-only and MBC1 (cpu_instrs is MBC1). Other MBCs
 * are a later frontier item.
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
        case 0x00: c->mbc = 0; break;
        case 0x01: case 0x02: case 0x03: c->mbc = 1; break;
        default:
            c->mbc = 1; /* best-effort: treat unknown as MBC1 for now */
            fprintf(stderr, "cart: unhandled MBC type 0x%02X, assuming MBC1\n", type);
            break;
    }
    c->has_battery = (type == 0x03);

    u8 rom_size_code = rom[0x148];
    c->rom_banks = 2 << rom_size_code;            /* 32KB << n */
    if ((size_t)c->rom_banks * 0x4000 > c->rom_size)
        c->rom_banks = (int)(c->rom_size / 0x4000);
    if (c->rom_banks < 2) c->rom_banks = 2;

    u8 ram_size_code = rom[0x149];
    c->ram_size = (ram_size_code < (int)(sizeof(RAM_SIZE_TABLE) / sizeof(int)))
                      ? (size_t)RAM_SIZE_TABLE[ram_size_code]
                      : 0;
    if (c->ram_size) {
        c->ram = calloc(1, c->ram_size);
        c->ram_banks = (int)(c->ram_size / 0x2000);
        if (c->ram_banks < 1) c->ram_banks = 1;
    }

    c->bank_lo = 1;
    c->bank_hi = 0;
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

static u32 rom_offset(Cart *c, int bank, u16 addr) {
    bank &= (c->rom_banks - 1);
    return (u32)bank * 0x4000 + (addr & 0x3FFF);
}

u8 cart_read(GB *gb, u16 addr) {
    Cart *c = &gb->cart;
    if (addr < 0x4000) {
        int bank = 0;
        if (c->mbc == 1 && c->mode == 1)
            bank = (c->bank_hi << 5) & (c->rom_banks - 1);
        return c->rom[rom_offset(c, bank, addr)];
    }
    if (addr < 0x8000) {
        int bank = 1;
        if (c->mbc == 1) {
            bank = (c->bank_hi << 5) | (c->bank_lo & 0x1F);
            if ((c->bank_lo & 0x1F) == 0) bank |= 1; /* 0 -> 1 in 5-bit field */
        }
        return c->rom[rom_offset(c, bank, addr)];
    }
    if (addr >= 0xA000 && addr < 0xC000) {
        if (!c->ram || !c->ram_enable) return 0xFF;
        int rbank = (c->mbc == 1 && c->mode == 1) ? c->bank_hi : 0;
        if (c->ram_banks) rbank &= (c->ram_banks - 1);
        u32 off = (u32)rbank * 0x2000 + (addr & 0x1FFF);
        if (off < c->ram_size) return c->ram[off];
        return 0xFF;
    }
    return 0xFF;
}

void cart_write(GB *gb, u16 addr, u8 val) {
    Cart *c = &gb->cart;
    if (c->mbc == 1) {
        if (addr < 0x2000) {
            c->ram_enable = (val & 0x0F) == 0x0A;
        } else if (addr < 0x4000) {
            c->bank_lo = val & 0x1F;
            if (c->bank_lo == 0) c->bank_lo = 1;
        } else if (addr < 0x6000) {
            c->bank_hi = val & 0x03;
        } else if (addr < 0x8000) {
            c->mode = val & 0x01;
        } else if (addr >= 0xA000 && addr < 0xC000) {
            if (c->ram && c->ram_enable) {
                int rbank = (c->mode == 1) ? c->bank_hi : 0;
                if (c->ram_banks) rbank &= (c->ram_banks - 1);
                u32 off = (u32)rbank * 0x2000 + (addr & 0x1FFF);
                if (off < c->ram_size) c->ram[off] = val;
            }
        }
        return;
    }
    /* ROM-only cart: writable external RAM only if present */
    if (addr >= 0xA000 && addr < 0xC000 && c->ram) {
        u32 off = addr & 0x1FFF;
        if (off < c->ram_size) c->ram[off] = val;
    }
}
