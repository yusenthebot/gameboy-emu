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
    /* MBC banking state (interpretation depends on mbc) */
    bool ram_enable;
    u8  bank_lo;        /* MBC1: 5-bit BANK1 · MBC2: 4-bit ROM bank · MBC5: ROM bank low 8 */
    u8  bank_hi;        /* MBC1: 2-bit BANK2 · MBC5: ROM bank bit 8 */
    u8  ram_bank;       /* MBC5: 4-bit RAM bank · MBC3: RAM bank (0-3) or RTC select (8-C) */
    u8  mode;           /* MBC1: 0 = ROM banking, 1 = RAM/advanced banking */
    u8  rtc[5];         /* MBC3 real-time clock: S, M, H, DL, DH */
    char title[17];
    bool has_battery;
} Cart;

/* APU - sound. ch index: 0=sq1(sweep) 1=sq2 2=wave 3=noise. */
typedef struct Apu {
    bool power;             /* NR52 bit 7 */
    u8   fs_step;           /* frame sequencer step 0-7 */
    bool div_bit_prev;      /* for the DIV-bit-12 falling-edge clock */

    bool ch_on[4];          /* channel active (NR52 status bits 0-3) */
    bool ch_dac[4];         /* DAC enabled for the channel */
    u16  length[4];         /* length counter (max 64 for sq/noise, 256 for wave) */
    bool length_en[4];      /* NRx4 bit 6 */

    /* volume envelope (ch 0,1,3) */
    u8   env_period[4], env_timer[4], env_vol[4];
    bool env_dir[4];

    /* frequency sweep (ch 0) */
    u8   sweep_period, sweep_timer, sweep_shift;
    bool sweep_dir, sweep_enabled, sweep_neg_used;
    u16  sweep_shadow;

    u8   reg[0x17];         /* NR10..NR52 raw = FF10..FF26 */
    u8   wave[16];          /* FF30..FF3F */

    /* channel output synthesis (audible sample generation) */
    int  sq_timer[2];       /* ch1/ch2 square frequency timer (T-cycles) */
    u8   sq_step[2];        /* duty step 0..7 */
    int  wv_timer;          /* ch3 wave frequency timer */
    u8   wv_pos;            /* wave sample index 0..31 */
    int  ns_timer;          /* ch4 noise frequency timer */
    u16  lfsr;              /* ch4 15-bit LFSR */
    int  sample_acc;        /* T-cycle accumulator for the output sample rate */
} Apu;

typedef struct GB {
    /* CPU registers */
    u8 a, f, b, c, d, e, h, l;
    u16 sp, pc;

    bool ime;               /* interrupt master enable */
    bool ime_pending;       /* EI sets this; IME turns on after next instr */
    bool halted;
    bool halt_bug;          /* next fetch does not advance PC */
    bool stopped;
    bool locked;            /* undefined opcode hung the CPU (only the clock runs) */
    bool double_speed;      /* CGB KEY1 double-speed: CPU 2x vs PPU/APU (crystal) */

    /* Memory regions */
    u8 vram[0x4000];        /* 2 banks (CGB); DMG uses bank 0 only */
    u8 wram[0x8000];        /* 8 banks (CGB SVBK); DMG uses banks 0-1 (8KB) */
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
    bool tima_reloaded;     /* reload happened in the current M-cycle (for write quirks) */

    /* APU */
    Apu apu;

    /* Serial */
    u8 sb, sc;

    /* PPU */
    u32 ppu_dot;            /* dot within current frame [0,70224) */
    u8  ly, lyc, stat, lcdc, scx, scy, wx, wy, bgp, obp0, obp1;
    u8  mode;               /* current PPU mode 0..3 */
    bool ly_coin;           /* LY==LYC coincidence, frozen while LCD is off */
    bool lcd_on_frame;      /* first frame after LCD enable (LY=0 mode-2 reads as 0) */
    bool stat_line;         /* STAT interrupt line (for rising-edge detection) */
    int  mode3_obj_pen;     /* per-line object mode-3 penalty in dots (cached at mode-3 start) */
    u8  win_line;           /* window internal line counter */
    u8  fb[160 * 144];      /* DMG: rendered shade indices 0..3 (0=light,3=dark) */
    u32 fb_rgb[160 * 144];  /* CGB: rendered RGB888 (also the frontend's color buffer) */
    bool frame_ready;       /* set when a full frame has been rendered */
    u64 frame_count;        /* frames completed (entered VBlank) */

    /* CGB (Game Boy Color) */
    bool cgb;               /* CGB mode (cart header 0x143 = 0x80/0xC0) */
    u8  vbk;                /* VRAM bank select (FF4F bit 0) */
    u8  svbk;               /* WRAM bank select (FF70 bits 0-2) */
    u8  key1;               /* double-speed prepare/status (FF4D) */
    u8  bcps, ocps;         /* BG/OBJ palette index + auto-increment (FF68/FF6A) */
    u8  bgpal[64];          /* 8 BG palettes x 4 colors x RGB555 (FF69) */
    u8  objpal[64];         /* 8 OBJ palettes x 4 colors x RGB555 (FF6B) */

    /* CGB VRAM DMA / HDMA (FF51-FF55) */
    u16 hdma_src, hdma_dst; /* current source / VRAM-relative dest */
    u8  hdma_len;           /* remaining 0x10-byte blocks - 1 (0x7F = idle) */
    bool hdma_active;       /* an HBlank-driven transfer is in progress */

    /* OAM DMA (cycle-accurate: 160 M-cycles, OAM locked during transfer) */
    u8  dma_reg;            /* last value written to FF46 */
    u16 dma_src;            /* active source base */
    u16 dma_pending_src;    /* source for a (re)start after the startup delay */
    int dma_pos;            /* next byte index 0..160 (160 = done) */
    int dma_start;          /* startup-delay M-cycles before (re)start */
    bool dma_running;       /* transfer in progress -> OAM locked to CPU */
    u8  dma_bus_val;        /* last byte the DMA read (seen on bus conflicts) */
    bool dma_reading;       /* guard: the DMA's own source fetch (no conflict) */

    /* Joypad */
    u8 joyp_select;
    u8 buttons;             /* bit set = pressed */

    /* Timing / control */
    u64 cycles;             /* total CPU T-cycles elapsed (2x crystal in double-speed) */
    u64 sys_cycles;         /* crystal/system clocks (LCD time), speed-independent */

    /* Serial capture for headless test harness */
    char serial_log[1 << 16];
    size_t serial_len;
} GB;

/* cart.c */
int  cart_load(GB *gb, const char *path);
void cart_free(GB *gb);
u8   cart_read(GB *gb, u16 addr);
void cart_write(GB *gb, u16 addr, u8 val);
int  cart_save_battery(GB *gb, const char *path);   /* persist cart RAM (+RTC) */
int  cart_load_battery(GB *gb, const char *path);

/* bus.c */
u8   bus_read(GB *gb, u16 addr);
void bus_write(GB *gb, u16 addr, u8 val);
void hdma_hblank_step(GB *gb);          /* CGB HBlank VRAM DMA: one block per HBlank */
int  fifo_bg_line(GB *g, int y, u8 *out);  /* pixel-FIFO BG renderer (T-cycle PPU spike) */
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
bool ppu_oam_accessible(GB *gb);   /* false during PPU modes 2/3 (CPU sees 0xFF) */
bool ppu_vram_accessible(GB *gb);  /* false during PPU mode 3 (CPU sees 0xFF) */

/* png.c */
int png_write_gray(const char *path, int w, int h, const u8 *indices);

/* apu.c */
void apu_init(GB *gb);
void apu_tick(GB *gb, int tcycles);
u8   apu_read(GB *gb, u16 addr);
void apu_write(GB *gb, u16 addr, u8 val);
int  apu_drain_samples(GB *gb, i16 *out, int max_pairs);  /* returns stereo pairs drained */
void apu_activity_reset(void);     /* Gambatte outaudio: reset the activity window */
int  apu_activity_varied(void);    /* 1 if the APU output varied (not silent) */
#define APU_SAMPLE_RATE 48000

/* state.c */
int gb_save_state(GB *gb, const char *path);
int gb_load_state(GB *gb, const char *path);
size_t gb_snapshot_size(GB *gb);
void   gb_snapshot(GB *gb, u8 *buf);     /* in-memory snapshot (for the rewind ring) */
void   gb_restore(GB *gb, const u8 *buf);

/* disasm.c / debug.c */
int  disasm(GB *gb, u16 addr, char *buf, size_t sz);  /* -> instruction length */
void debugger_repl(GB *gb);                            /* interactive debugger (stdin) */

/* cpu.c */
void cpu_init_postboot(GB *gb);
int  cpu_step(GB *gb);          /* returns T-cycles consumed */
void cpu_request_interrupt(GB *gb, u8 mask);

#endif /* GB_H */
