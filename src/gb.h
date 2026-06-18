/* gb.h - core types and state for the Game Boy (DMG) emulator.
 *
 * Round 1 floor: instruction-stepped SM83 core good enough to pass
 * Blargg cpu_instrs. Subsystems (timer/ppu/serial) are ticked by the
 * T-cycles each instruction consumes. Later rounds migrate this toward
 * per-M-cycle ticking for true sub-instruction accuracy.
 */
#ifndef GB_H
#define GB_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;

/* Interrupt bit positions (in IE / IF). */
enum {
    INT_VBLANK = 0x01,
    INT_STAT   = 0x02,
    INT_TIMER  = 0x04,
    INT_SERIAL = 0x08,
    INT_JOYPAD = 0x10,
};

/* CPU flag bits in register F. */
enum {
    FLAG_Z = 0x80,
    FLAG_N = 0x40,
    FLAG_H = 0x20,
    FLAG_C = 0x10,
};

typedef struct Cart {
    u8 *rom;
    size_t rom_size;
    u8 *ram;
    size_t ram_size;
    int mbc;            /* 0 = none, 1 = MBC1, etc. */
    int rom_banks;
    int ram_banks;
    /* MBC1 banking state */
    bool ram_enable;
    u8  bank_lo;        /* 5-bit low bank number */
    u8  bank_hi;        /* 2-bit upper bits (RAM bank or ROM upper) */
    u8  mode;           /* 0 = ROM banking, 1 = RAM banking */
    char title[17];
    bool has_battery;
} Cart;

typedef struct GB {
    /* CPU registers */
    u8 a, f, b, c, d, e, h, l;
    u16 sp, pc;

    bool ime;               /* interrupt master enable */
    bool ime_pending;       /* EI sets this; IME turns on after next instr */
    bool halted;
    bool halt_bug;          /* next fetch does not advance PC */
    bool stopped;

    /* Memory regions */
    u8 vram[0x2000];
    u8 wram[0x2000];
    u8 oam[0xA0];
    u8 hram[0x7F];
    u8 io[0x80];            /* 0xFF00..0xFF7F raw store for simple regs */
    u8 ie;                  /* 0xFFFF */

    Cart cart;

    /* Timer */
    u16 div_counter;        /* internal 16-bit divider; DIV = high byte */
    u8  tima, tma, tac;
    bool tima_overflow;     /* delayed reload state */
    u8  tima_reload_delay;

    /* Serial */
    u8 sb, sc;

    /* PPU */
    u32 ppu_dot;            /* dot within current frame [0,70224) */
    u8  ly, lyc, stat, lcdc, scx, scy, wx, wy, bgp, obp0, obp1;
    u8  mode;               /* current PPU mode 0..3 */
    u8  win_line;           /* window internal line counter */
    u8  fb[160 * 144];      /* rendered shade indices 0..3 (0=light,3=dark) */
    bool frame_ready;       /* set when a full frame has been rendered */
    u64 frame_count;        /* frames completed (entered VBlank) */

    /* OAM DMA (cycle-accurate: 160 M-cycles, OAM locked during transfer) */
    u8  dma_reg;            /* last value written to FF46 */
    u16 dma_src;            /* active source base */
    u16 dma_pending_src;    /* source for a (re)start after the startup delay */
    int dma_pos;            /* next byte index 0..160 (160 = done) */
    int dma_start;          /* startup-delay M-cycles before (re)start */
    bool dma_running;       /* transfer in progress -> OAM locked to CPU */

    /* Joypad */
    u8 joyp_select;
    u8 buttons;             /* bit set = pressed */

    /* Timing / control */
    u64 cycles;             /* total T-cycles elapsed */

    /* Serial capture for headless test harness */
    char serial_log[1 << 16];
    size_t serial_len;
} GB;

/* cart.c */
int  cart_load(GB *gb, const char *path);
void cart_free(GB *gb);
u8   cart_read(GB *gb, u16 addr);
void cart_write(GB *gb, u16 addr, u8 val);

/* bus.c */
u8   bus_read(GB *gb, u16 addr);
void bus_write(GB *gb, u16 addr, u8 val);
void dma_tick(GB *gb, int tcycles);

/* timer.c */
void timer_tick(GB *gb, int tcycles);
u8   timer_read(GB *gb, u16 addr);
void timer_write(GB *gb, u16 addr, u8 val);

/* serial.c */
void serial_tick(GB *gb, int tcycles);
u8   serial_read(GB *gb, u16 addr);
void serial_write(GB *gb, u16 addr, u8 val);

/* ppu.c */
void ppu_tick(GB *gb, int tcycles);
u8   ppu_read(GB *gb, u16 addr);
void ppu_write(GB *gb, u16 addr, u8 val);

/* png.c */
int png_write_gray(const char *path, int w, int h, const u8 *indices);

/* cpu.c */
void cpu_init_postboot(GB *gb);
int  cpu_step(GB *gb);          /* returns T-cycles consumed */
void cpu_request_interrupt(GB *gb, u8 mask);

#endif /* GB_H */
