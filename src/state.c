/* state.c - save-states (full machine snapshot to a file).
 *
 * Serializes the entire GB struct plus the cartridge RAM. The cartridge ROM and
 * the heap pointers (rom/ram) are NOT part of the snapshot: they belong to the
 * loaded ROM, so on restore we keep the live buffers and only copy back the
 * banking state + RAM contents. Loading a snapshot and continuing must be
 * bit-identical to never having stopped (verified by the determinism gate test).
 */
#include "gb.h"
#include <stdio.h>
#include <string.h>

#define STATE_MAGIC   0x53534247u   /* 'GBSS' little-endian */
#define STATE_VERSION 1u

int gb_save_state(GB *g, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    u32 magic = STATE_MAGIC, ver = STATE_VERSION;
    u64 ramsz = (u64)g->cart.ram_size;
    int ok = fwrite(&magic, 4, 1, f) == 1
          && fwrite(&ver, 4, 1, f) == 1
          && fwrite(&ramsz, 8, 1, f) == 1
          && fwrite(g, sizeof(GB), 1, f) == 1;
    if (ok && ramsz && g->cart.ram) ok = fwrite(g->cart.ram, 1, ramsz, f) == ramsz;
    fclose(f);
    return ok ? 0 : -2;
}

/* In-memory snapshot/restore (no file I/O) — the basis for the rewind ring.
 * Same contract as save/load: the ROM/RAM heap buffers stay live; only the
 * machine state + cart RAM contents move. */
size_t gb_snapshot_size(GB *g) { return sizeof(GB) + g->cart.ram_size; }

void gb_snapshot(GB *g, u8 *buf) {
    memcpy(buf, g, sizeof(GB));
    if (g->cart.ram_size && g->cart.ram) memcpy(buf + sizeof(GB), g->cart.ram, g->cart.ram_size);
}

void gb_restore(GB *g, const u8 *buf) {
    u8 *rom = g->cart.rom, *ram = g->cart.ram;
    size_t rom_size = g->cart.rom_size, ram_size = g->cart.ram_size;
    memcpy(g, buf, sizeof(GB));
    g->cart.rom = rom; g->cart.rom_size = rom_size;
    g->cart.ram = ram; g->cart.ram_size = ram_size;
    if (ram_size && ram) memcpy(ram, buf + sizeof(GB), ram_size);
}

int gb_load_state(GB *g, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    u32 magic = 0, ver = 0; u64 ramsz = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != STATE_MAGIC ||
        fread(&ver, 4, 1, f) != 1 || ver != STATE_VERSION ||
        fread(&ramsz, 8, 1, f) != 1) { fclose(f); return -2; }

    /* The ROM/RAM heap buffers belong to the live cart, not the snapshot. */
    u8 *rom = g->cart.rom, *ram = g->cart.ram;
    size_t rom_size = g->cart.rom_size, ram_size = g->cart.ram_size;

    if (fread(g, sizeof(GB), 1, f) != 1) { fclose(f); return -2; }

    g->cart.rom = rom; g->cart.rom_size = rom_size;
    g->cart.ram = ram; g->cart.ram_size = ram_size;
    if (ramsz && ram && ramsz <= (u64)ram_size)
        if (fread(ram, 1, ramsz, f) != ramsz) { fclose(f); return -2; }
    fclose(f);
    return 0;
}
