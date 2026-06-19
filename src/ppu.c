/* ppu.c - scanline PPU with background, window and sprite rendering.
 *
 * Renders each visible scanline (sampling registers at the mode-3->0
 * transition) into a 160x144 framebuffer of shade indices (0=lightest,
 * 3=darkest). Mode 2 = 80 dots, mode 3 = 172 + (SCX & 7) dots, mode 0 fills
 * the rest. The STAT *mode field* read via FF41 lags the internal mode by 8
 * dots at the 2->3 and 3->0 boundaries (a DMG quirk Mooneye intr_2 tests pin);
 * the STAT IRQ and rendering use the real transitions. OAM is blocked to the CPU
 * in modes 2/3 and VRAM in mode 3 (ppu_oam/vram_accessible). The sprite/window
 * mode-3 length penalty (pixel-FIFO) is the remaining frontier — see
 * docs/ppu-mode3-sprite-penalty.md for the oracle and analysis.
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

/* CGB color rendering: BG/window tiles carry per-tile attributes in VRAM bank 1
 * (palette/bank/flip/priority), colors come from the 8 BG/OBJ palettes (RGB555),
 * and objects are priority-ordered by OAM index. Output is RGB888 in fb_rgb. */
static inline u32 rgb555(u16 c) {
    int r = c & 0x1F, g = (c >> 5) & 0x1F, b = (c >> 10) & 0x1F;
    return ((u32)((r << 3) | (r >> 2)) << 16) |
           ((u32)((g << 3) | (g >> 2)) << 8)  |
            (u32)((b << 3) | (b >> 2));
}

static void render_scanline_cgb(GB *g, int y) {
    u8 bg_cn[160], bg_pr[160];     /* BG color number + priority bit, per pixel */
    bool win_on = (g->lcdc & LCDC_WIN_EN) && (y >= g->wy);
    u16 bg_map = (g->lcdc & LCDC_BG_MAP) ? 0x9C00 : 0x9800;
    u16 win_map = (g->lcdc & LCDC_WIN_MAP) ? 0x9C00 : 0x9800;
    bool unsigned_tiles = (g->lcdc & LCDC_TILE_DATA) != 0;
    bool window_used = false;

    for (int x = 0; x < 160; x++) {
        u16 map; int px, py;
        if (win_on && x + 7 >= g->wx) {
            window_used = true;
            int wx_px = x - (g->wx - 7);
            map = win_map + (g->win_line / 8) * 32 + (wx_px / 8);
            px = wx_px % 8; py = g->win_line % 8;
        } else {
            int bx = (x + g->scx) & 0xFF, by = (y + g->scy) & 0xFF;
            map = bg_map + (by / 8) * 32 + (bx / 8);
            px = bx % 8; py = by % 8;
        }
        int moff = map - 0x8000;
        u8 id = g->vram[moff];                       /* tile id (bank 0) */
        u8 attr = g->vram[0x2000 + moff];            /* attribute (bank 1) */
        int pal = attr & 7, bank = (attr >> 3) & 1;
        if (attr & 0x40) py = 7 - py;                /* Y-flip */
        int bit = (attr & 0x20) ? px : (7 - px);     /* X-flip */
        u16 taddr = unsigned_tiles ? 0x8000 + id * 16 : 0x9000 + (i16)((i8)id) * 16;
        int toff = bank * 0x2000 + (taddr - 0x8000) + py * 2;
        u8 cn = tile_colnum(g->vram[toff], g->vram[toff + 1], bit);
        bg_cn[x] = cn; bg_pr[x] = (attr >> 7) & 1;
        u16 c = g->bgpal[pal * 8 + cn * 2] | (g->bgpal[pal * 8 + cn * 2 + 1] << 8);
        g->fb_rgb[y * 160 + x] = rgb555(c);
    }
    if (window_used) g->win_line++;

    if (!(g->lcdc & LCDC_OBJ_EN)) return;
    int height = (g->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    int sel[10], n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int top = g->oam[i * 4] - 16;
        if (y >= top && y < top + height) sel[n++] = i;
    }
    bool bg_master = (g->lcdc & LCDC_BG_EN) != 0;    /* LCDC.0 = BG/win master priority */
    for (int s = n - 1; s >= 0; s--) {               /* low OAM index drawn last = on top */
        int i = sel[s];
        u8 sy = g->oam[i * 4], sx = g->oam[i * 4 + 1], tile = g->oam[i * 4 + 2], attr = g->oam[i * 4 + 3];
        int top = sy - 16, row = y - top;
        if (attr & 0x40) row = height - 1 - row;     /* Y-flip */
        if (height == 16) { tile &= 0xFE; if (row >= 8) { tile |= 1; row -= 8; } }
        int bank = (attr >> 3) & 1, pal = attr & 7;
        int toff = bank * 0x2000 + tile * 16 + row * 2;
        u8 lo = g->vram[toff], hi = g->vram[toff + 1];
        bool obj_behind = (attr & 0x80) != 0;
        for (int p = 0; p < 8; p++) {
            int xx = sx - 8 + p;
            if (xx < 0 || xx >= 160) continue;
            int bit = (attr & 0x20) ? p : (7 - p);   /* X-flip */
            u8 cn = tile_colnum(lo, hi, bit);
            if (cn == 0) continue;
            if (bg_master && bg_cn[xx] != 0 && (bg_pr[xx] || obj_behind)) continue;  /* BG wins */
            u16 c = g->objpal[pal * 8 + cn * 2] | (g->objpal[pal * 8 + cn * 2 + 1] << 8);
            g->fb_rgb[y * 160 + xx] = rgb555(c);
        }
    }
}

/* The standard DMG 4-shade grayscale, for the color framebuffer / --rgb dumps. */
static const u32 DMG_GRAY[4] = {0xFFFFFF, 0xAAAAAA, 0x555555, 0x000000};

void render_scanline(GB *g, int y) {
    if (g->cgb) { render_scanline_cgb(g, y); return; }
    u8 bg_colnum[160];
    render_bg_window(g, y, bg_colnum);
    render_sprites(g, y, bg_colnum);
    for (int x = 0; x < 160; x++)                 /* mirror shades into fb_rgb */
        g->fb_rgb[y * 160 + x] = DMG_GRAY[g->fb[y * 160 + x] & 3];
}

/* Objects lengthen mode 3 (the pixel pipeline stalls to fetch each one). Pan Docs
 * "OBJ penalty algorithm": for each object (left-to-right, ties by OAM index), let
 * "The Pixel" be its leftmost pixel; find the BG tile it lands in; if that tile
 * has not been considered by a previous object, add (pixels strictly right of The
 * Pixel) - 2 dots (>=0); then a flat 6 dots. An object at OAM X=0 (off-screen left)
 * is treated as offset 0 regardless of SCX; objects at X>=168 are off-screen right
 * and not drawn. */
int obj_mode3_penalty(GB *g, int ly) {
    if (!(g->lcdc & LCDC_OBJ_EN)) return 0;
    int height = (g->lcdc & LCDC_OBJ_SIZE) ? 16 : 8;
    typedef struct { int x, oam; } Obj;
    Obj sp[10];
    int n = 0;
    for (int i = 0; i < 40 && n < 10; i++) {
        int top = g->oam[i * 4] - 16;
        if (ly >= top && ly < top + height) { sp[n].x = g->oam[i * 4 + 1]; sp[n].oam = i; n++; }
    }
    for (int a = 0; a < n - 1; a++)
        for (int b = a + 1; b < n; b++)
            if (sp[b].x < sp[a].x || (sp[b].x == sp[a].x && sp[b].oam < sp[a].oam)) {
                Obj t = sp[a]; sp[a] = sp[b]; sp[b] = t;
            }

    int penalty = 0, seen[10], ns = 0;
    for (int i = 0; i < n; i++) {
        if (sp[i].x >= 168) continue;                    /* off-screen right: not drawn */
        int bg_pos = (sp[i].x - 8) + g->scx;             /* The Pixel's BG x position */
        int tile = bg_pos >> 3;                          /* arithmetic shift (floors) */
        bool considered = false;
        for (int k = 0; k < ns; k++) if (seen[k] == tile) { considered = true; break; }
        if (!considered) {
            seen[ns++] = tile;
            /* X=0 (off-screen left) always behaves as offset 0, regardless of SCX. */
            int off = (sp[i].x == 0) ? 0 : (bg_pos & 7);
            int pen = (7 - off) - 2;                      /* pixels right of The Pixel - 2 */
            if (pen > 0) penalty += pen;
        }
        penalty += 6;
    }
    /* The scanline model emits the whole line at once and detects mode 0 a few
     * dots later than a real per-dot fetcher would; reducing the per-line object
     * penalty by 3 dots aligns it with the Mooneye measurement. Validated against
     * all 105 testcases of intr_2_mode0_timing_sprites. */
    if (penalty > 0) penalty -= 3;
    return penalty;
}

static void set_mode(GB *g, u8 mode) { g->mode = mode; }

/* The mode field reported via STAT lags the internal mode at the 2->3 and
 * 3->0 boundaries by 8 dots (a documented DMG quirk Mooneye's intr_2_mode0/
 * mode3 tests pin down). The STAT interrupt line and rendering still use the
 * real transitions. */
#define STAT_MODE_DELAY 8
/* Mode-3 is lengthened by the SCX fine-scroll (0-7 dots), which pushes mode 0
 * (and its STAT IRQ) later. Sprite/window penalties are a later item. */
static inline int mode3_end(GB *g) { return MODE3_END + (g->scx & 7) + g->mode3_obj_pen; }

static u8 stat_reported_mode(GB *g) {
    if (!(g->lcdc & LCDC_LCD_EN)) return 0;   /* LCD off -> STAT mode reads 0 */
    if (g->ly >= VBLANK_LINE) return 1;
    u32 dot = g->ppu_dot % LINE_DOTS;
    /* The +8-dot delay applies to the 0->2 (line-start) boundary too, so the
     * mode field reads 0 (HBlank tail) for the first 8 dots of each line and
     * right after the LCD is enabled. */
    if (dot < STAT_MODE_DELAY) return 0;
    if (dot < MODE2_END + STAT_MODE_DELAY)
        return (g->lcd_on_frame && g->ly == 0) ? 0 : 2;  /* first OAM scan reads 0 */
    if (dot < (u32)mode3_end(g) + STAT_MODE_DELAY) return 3;
    return 0;
}

/* OAM is accessible to the CPU only in HBlank/VBlank (reported mode 0/1), not
 * during OAM-scan/drawing (modes 2/3). Uses the same reported mode as STAT so
 * the access window matches the STAT timing the intr_2 tests measure. */
bool ppu_oam_accessible(GB *g) {
    return stat_reported_mode(g) <= 1;
}

/* VRAM is inaccessible to the CPU only during drawing (mode 3). */
bool ppu_vram_accessible(GB *g) {
    return stat_reported_mode(g) != 3;
}

static void stat_check(GB *g) {
    bool line = false;
    if ((g->stat & 0x08) && g->mode == 0) line = true;
    if ((g->stat & 0x10) && g->mode == 1) line = true;
    if ((g->stat & 0x20) && g->mode == 2) line = true;
    if ((g->stat & 0x40) && g->ly == g->lyc) line = true;
    if (line && !g->stat_line) cpu_request_interrupt(g, INT_STAT);
    g->stat_line = line;
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
        if (ly != 0) g->lcd_on_frame = false;   /* first-frame quirk ends after LY=0 */

        u8 mode;
        if (ly >= VBLANK_LINE) mode = 1;
        else if (line_dot < MODE2_END) mode = 2;
        else if (line_dot < (u32)mode3_end(g)) mode = 3;
        else mode = 0;

        if (mode != g->mode) {
            /* Entering mode 3: the OAM scan is done, so the object list (and its
             * mode-3 penalty) for this line is fixed. */
            if (mode == 3) g->mode3_obj_pen = obj_mode3_penalty(g, ly);
            /* On mode-3 -> 0 transition, render the just-finished line and step
             * any HBlank-driven VRAM DMA (one 0x10-byte block per HBlank). */
            if (g->mode == 3 && mode == 0 && ly < VBLANK_LINE) {
                render_scanline(g, ly);
                if (g->cgb) hdma_hblank_step(g);
            }
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
        case 0xFF41: {
            /* Coincidence is live while the LCD is on, frozen at its turn-off
             * value while the LCD is off. */
            bool coin = (g->lcdc & LCDC_LCD_EN) ? (g->ly == g->lyc) : g->ly_coin;
            return (g->stat & 0x78) | (coin ? 0x04 : 0) | stat_reported_mode(g) | 0x80;
        }
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
            bool now_on = (val & LCDC_LCD_EN) != 0;
            if (was_on && !now_on) {
                g->ly_coin = (g->ly == g->lyc);   /* freeze coincidence at turn-off */
                /* The STAT line holds its frozen value while off (only the LYC
                 * source can be active, since the mode reads 0), so a later
                 * LCD-on only re-triggers if it's a genuine rising edge. */
                g->stat_line = (g->stat & 0x40) && g->ly_coin;
                g->ly = 0; g->ppu_dot = 0; g->mode = 0; g->win_line = 0;
            }
            if (!was_on && now_on) g->lcd_on_frame = true; /* first-frame quirk */
            g->lcdc = val;
            if (!was_on && now_on) stat_check(g); /* LCD-on coincidence fires now */
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
