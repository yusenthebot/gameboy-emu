/* ppu.c - scanline PPU with background, window and sprite rendering.
 *
 * Renders each visible scanline (sampling registers at the mode-3->0
 * transition) into a 160x144 framebuffer of shade indices (0=lightest,
 * 3=darkest). Mode timing is fixed-length per line (OAM 80 / draw 172 /
 * HBlank 204 dots); variable mode-3 length and VRAM/OAM access blocking
 * are a later (sub-instruction timing) frontier. Enough to pass dmg-acid2.
 */
#include "gb.h"
#include <string.h>

#define LINE_DOTS 456
#define LINES 154
#define VBLANK_LINE 144
#define MODE2_END 80
#define MODE3_END (80 + 172)

/* LCDC bits */
#define LCDC_BG_EN     0x01
#define LCDC_OBJ_EN    0x02
#define LCDC_OBJ_SIZE  0x04
#define LCDC_BG_MAP    0x08
#define LCDC_TILE_DATA 0x10
#define LCDC_WIN_EN    0x20
#define LCDC_WIN_MAP   0x40
#define LCDC_LCD_EN    0x80

static inline u8 vram(GB *g, u16 addr) { return g->vram[addr - 0x8000]; }

/* Decode one pixel's 2-bit color number from a tile row. */
static inline u8 tile_colnum(u8 lo, u8 hi, int bit) {
    return (u8)((((hi >> bit) & 1) << 1) | ((lo >> bit) & 1));
}

static void render_bg_window(GB *g, int y, u8 *bg_colnum_out) {
    bool bg_on = (g->lcdc & LCDC_BG_EN) != 0;
    bool win_on = (g->lcdc & LCDC_WIN_EN) && bg_on && (y >= g->wy);
    u16 bg_map = (g->lcdc & LCDC_BG_MAP) ? 0x9C00 : 0x9800;
    u16 win_map = (g->lcdc & LCDC_WIN_MAP) ? 0x9C00 : 0x9800;
    bool unsigned_tiles = (g->lcdc & LCDC_TILE_DATA) != 0;
    bool window_used = false;

    for (int x = 0; x < 160; x++) {
        u8 colnum;
        bool in_window = win_on && (x + 7 >= g->wx);
        if (!bg_on) {
            colnum = 0;                 /* BG off -> color 0 (white) */
        } else if (in_window) {
            window_used = true;
            int wx_px = x - (g->wx - 7);
            int wy_px = g->win_line;
            u16 map = win_map + (wy_px / 8) * 32 + (wx_px / 8);
            u8 idx = vram(g, map);
            u16 taddr = unsigned_tiles ? 0x8000 + idx * 16
                                       : 0x9000 + (i16)((i8)idx) * 16;
            u8 lo = vram(g, taddr + (wy_px % 8) * 2);
            u8 hi = vram(g, taddr + (wy_px % 8) * 2 + 1);
            colnum = tile_colnum(lo, hi, 7 - (wx_px % 8));
        } else {
            int bx = (x + g->scx) & 0xFF;
            int by = (y + g->scy) & 0xFF;
            u16 map = bg_map + (by / 8) * 32 + (bx / 8);
            u8 idx = vram(g, map);
            u16 taddr = unsigned_tiles ? 0x8000 + idx * 16
                                       : 0x9000 + (i16)((i8)idx) * 16;
            u8 lo = vram(g, taddr + (by % 8) * 2);
            u8 hi = vram(g, taddr + (by % 8) * 2 + 1);
            colnum = tile_colnum(lo, hi, 7 - (bx % 8));
        }
        bg_colnum_out[x] = colnum;
        g->fb[y * 160 + x] = (g->bgp >> (colnum * 2)) & 3;
    }
    if (window_used) g->win_line++;
}

typedef struct { u8 y, x, tile, attr, oam; } Sprite;

static void render_sprites(GB *g, int y, const u8 *bg_colnum) {
    if (!(g->lcdc & LCDC_OBJ_EN)) return;
    int height = (g->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;

    /* Select up to 10 sprites on this line, in OAM order. */
    Sprite sel[10];
    int n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        u8 sy = g->oam[i * 4 + 0];
        int top = sy - 16;
        if (y >= top && y < top + height) {
            sel[n].y = sy;
            sel[n].x = g->oam[i * 4 + 1];
            sel[n].tile = g->oam[i * 4 + 2];
            sel[n].attr = g->oam[i * 4 + 3];
            sel[n].oam = (u8)i;
            n++;
        }
    }
    /* Draw priority: lower X wins; tie broken by lower OAM index. Draw from
     * lowest priority to highest so the winner ends up on top. */
    for (int a = 0; a < n - 1; a++)
        for (int b = a + 1; b < n; b++)
            if (sel[b].x < sel[a].x ||
                (sel[b].x == sel[a].x && sel[b].oam < sel[a].oam)) {
                Sprite t = sel[a]; sel[a] = sel[b]; sel[b] = t;
            }

    for (int s = n - 1; s >= 0; s--) {
        Sprite *sp = &sel[s];
        int top = sp->y - 16;
        int row = y - top;
        if (sp->attr & 0x40) row = height - 1 - row;        /* Y flip */
        u8 tile = sp->tile;
        if (height == 16) { tile &= 0xFE; if (row >= 8) { tile |= 1; row -= 8; } }
        u16 taddr = 0x8000 + tile * 16 + row * 2;
        u8 lo = vram(g, taddr);
        u8 hi = vram(g, taddr + 1);
        u8 palette = (sp->attr & 0x10) ? g->obp1 : g->obp0;
        bool behind = (sp->attr & 0x80) != 0;

        for (int px = 0; px < 8; px++) {
            int sx = sp->x - 8 + px;
            if (sx < 0 || sx >= 160) continue;
            int bit = (sp->attr & 0x20) ? px : (7 - px);   /* X flip */
            u8 colnum = tile_colnum(lo, hi, bit);
            if (colnum == 0) continue;                      /* transparent */
            if (behind && bg_colnum[sx] != 0) continue;     /* BG priority */
            g->fb[y * 160 + sx] = (palette >> (colnum * 2)) & 3;
        }
    }
}

static void render_scanline(GB *g, int y) {
    u8 bg_colnum[160];
    render_bg_window(g, y, bg_colnum);
    render_sprites(g, y, bg_colnum);
}

static void set_mode(GB *g, u8 mode) { g->mode = mode; }

static void stat_check(GB *g) {
    bool line = false;
    if ((g->stat & 0x08) && g->mode == 0) line = true;
    if ((g->stat & 0x10) && g->mode == 1) line = true;
    if ((g->stat & 0x20) && g->mode == 2) line = true;
    if ((g->stat & 0x40) && g->ly == g->lyc) line = true;
    static bool prev;
    if (line && !prev) cpu_request_interrupt(g, INT_STAT);
    prev = line;
}

void ppu_tick(GB *g, int tcycles) {
    if (!(g->lcdc & LCDC_LCD_EN)) {
        g->ppu_dot = 0; g->ly = 0; g->mode = 0; g->win_line = 0;
        return;
    }
    for (int i = 0; i < tcycles; i++) {
        u32 line_dot = g->ppu_dot % LINE_DOTS;
        u8 ly = (u8)(g->ppu_dot / LINE_DOTS);
        g->ly = ly;

        u8 mode;
        if (ly >= VBLANK_LINE) mode = 1;
        else if (line_dot < MODE2_END) mode = 2;
        else if (line_dot < MODE3_END) mode = 3;
        else mode = 0;

        if (mode != g->mode) {
            /* On mode-3 -> 0 transition, render the just-finished line. */
            if (g->mode == 3 && mode == 0 && ly < VBLANK_LINE)
                render_scanline(g, ly);
            set_mode(g, mode);
            if (mode == 1) {
                cpu_request_interrupt(g, INT_VBLANK);
                g->frame_ready = true;
                g->frame_count++;
            }
        }
        if (ly == 0 && line_dot == 0) g->win_line = 0; /* reset window line at frame top */
        stat_check(g);

        g->ppu_dot++;
        if (g->ppu_dot >= LINE_DOTS * LINES) g->ppu_dot = 0;
    }
}

u8 ppu_read(GB *g, u16 addr) {
    switch (addr) {
        case 0xFF40: return g->lcdc;
        case 0xFF41: return (g->stat & 0x78) | (g->ly == g->lyc ? 0x04 : 0) | g->mode | 0x80;
        case 0xFF42: return g->scy;
        case 0xFF43: return g->scx;
        case 0xFF44: return g->ly;
        case 0xFF45: return g->lyc;
        case 0xFF47: return g->bgp;
        case 0xFF48: return g->obp0;
        case 0xFF49: return g->obp1;
        case 0xFF4A: return g->wy;
        case 0xFF4B: return g->wx;
        default:     return 0xFF;
    }
}

void ppu_write(GB *g, u16 addr, u8 val) {
    switch (addr) {
        case 0xFF40: {
            bool was_on = (g->lcdc & LCDC_LCD_EN) != 0;
            g->lcdc = val;
            if (was_on && !(val & LCDC_LCD_EN)) {
                g->ly = 0; g->ppu_dot = 0; g->mode = 0; g->win_line = 0;
            }
            break;
        }
        case 0xFF41: g->stat = val & 0x78; break;
        case 0xFF42: g->scy = val; break;
        case 0xFF43: g->scx = val; break;
        case 0xFF45: g->lyc = val; break;
        case 0xFF47: g->bgp = val; break;
        case 0xFF48: g->obp0 = val; break;
        case 0xFF49: g->obp1 = val; break;
        case 0xFF4A: g->wy = val; break;
        case 0xFF4B: g->wx = val; break;
        default: break;
    }
}
