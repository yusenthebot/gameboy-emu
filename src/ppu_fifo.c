/* ppu_fifo.c — pixel-FIFO background renderer: a spike toward a T-cycle-accurate PPU.
 *
 * The current PPU is a scanline renderer: pixels are exact, but mode-3 length
 * comes from a *calibrated* penalty (the +8 STAT delay, the -3 object fudge),
 * not from the pipeline itself. That M-cycle-level approximation is the ceiling
 * that blocks the sub-cycle timing tail (lcdon's 2 T-cycle quirk, the precise
 * STAT/OAM tests). The real unlock is a per-dot pixel FIFO where mode-3 length
 * *emerges* from fetcher stalls — the "FIFO 像素流水线" the goal names.
 *
 * This is the migration's first verifiable step: a background-only FIFO that
 * (a) reproduces the scanline renderer's pixels and (b) produces the canonical
 * BG mode-3 length of 172 + (SCX & 7) dots, with no fudge factor. Window and
 * sprite fetches (which extend mode 3) are the next steps; once the pipeline
 * drives the real PPU's timing, the calibration constants go away.
 *
 * Pipeline (per dot of mode 3):
 *   - fetcher: 2 dots per step, steps = get-tile-id, get-low, get-high, push.
 *     It pushes 8 pixels into the BG FIFO only when the FIFO has <= 8 pixels.
 *   - a "dummy" first tile fetch at mode-3 start is discarded (the 6-dot warm-up).
 *   - mixer: shifts out 1 pixel/dot whenever the FIFO is non-empty; the first
 *     SCX&7 shifted pixels are discarded (fine horizontal scroll).
 */
#include "gb.h"

static inline u8 fifo_vram(GB *g, u16 addr) { return g->vram[addr - 0x8000]; }

/* Render one BG+window scanline via the FIFO. Fills out[160] with shade
 * indices (0..3) and returns the mode-3 length in dots. The window mid-line
 * switch restarts the fetcher (a known mode-3 extender). Sprites not yet
 * modelled. (LCDC bit 0 assumed on.) */
int fifo_bg_line(GB *g, int y, u8 *out) {
    u16 bg_map  = (g->lcdc & 0x08) ? 0x9C00 : 0x9800;
    u16 win_map = (g->lcdc & 0x40) ? 0x9C00 : 0x9800;
    int unsigned_tiles = (g->lcdc & 0x10) != 0;
    int win_on = (g->lcdc & 0x20) && (g->lcdc & 0x01) && (y >= g->wy);
    int wx = g->wx, scx = g->scx;
    int by = (g->scy + y) & 0xFF, trow = by & 7, mapy = (by >> 3) & 0x1F;
    int wl = g->win_line, wtrow = wl & 7, wmapy = (wl >> 3) & 0x1F;

    /* Select up to 10 objects on this line (OAM order), precompute their row
     * bytes, and sort by (X, OAM index) so lower-X / lower-index win priority. */
    int obj_en = (g->lcdc & 0x02) != 0;
    int height = (g->lcdc & 0x04) ? 16 : 8;
    struct objsel { int x, fetched; u8 lo, hi, attr; } sp[10]; int oamidx[10]; int nsp = 0;
    if (obj_en) {
        for (int i = 0; i < 40 && nsp < 10; i++) {
            int sy = g->oam[i * 4], top = sy - 16;
            if (y < top || y >= top + height) continue;
            int row = y - top; u8 attr = g->oam[i * 4 + 3];
            if (attr & 0x40) row = height - 1 - row;
            int tile = g->oam[i * 4 + 2];
            if (height == 16) { tile &= 0xFE; if (row >= 8) { tile |= 1; row -= 8; } }
            u16 ta = 0x8000 + tile * 16 + row * 2;
            sp[nsp].x = g->oam[i * 4 + 1]; sp[nsp].attr = attr; sp[nsp].fetched = 0;
            sp[nsp].lo = fifo_vram(g, ta); sp[nsp].hi = fifo_vram(g, ta + 1);
            oamidx[nsp] = i; nsp++;
        }
        for (int a = 0; a < nsp - 1; a++)               /* stable sort by (x, oam) */
            for (int b = a + 1; b < nsp; b++)
                if (sp[b].x < sp[a].x || (sp[b].x == sp[a].x && oamidx[b] < oamidx[a])) {
                    struct objsel t = sp[a]; sp[a] = sp[b]; sp[b] = t;
                    int ti = oamidx[a]; oamidx[a] = oamidx[b]; oamidx[b] = ti;
                }
    }
    u8 obj_cn[8] = {0}, obj_pal[8] = {0}, obj_bh[8] = {0};   /* OBJ FIFO (8-deep) */

    u8 fifo[16]; int fhead = 0, flen = 0;
    int fetch_col = 0, fstate = 0, ftimer = 0;
    u16 ftaddr = 0; u8 flo = 0, fhi = 0;
    int warmup = 1, discard = scx & 7, in_win = 0;
    int seen[10], ns = 0, stall = 0;        /* object stall: mode-3 penalty emerges here */
    int out_x = 0, dot = 0;

    while (out_x < 160) {
        /* window trigger: the next visible pixel is inside the window -> the
         * fetcher restarts on the window map (FIFO cleared). */
        if (win_on && !in_win && out_x + 7 >= wx) {
            in_win = 1; flen = 0; fhead = 0; fstate = 0; ftimer = 0;
            fetch_col = 0; warmup = 1;
            discard = (wx < 7) ? (7 - wx) : 0;   /* window starting left of x=0 */
        }
        if (stall == 0 && ++ftimer >= 2) {       /* BG fetcher pauses during an object fetch */
            ftimer = 0;
            if (fstate == 0) {
                int mapx = in_win ? (fetch_col & 0x1F) : (((scx >> 3) + fetch_col) & 0x1F);
                int my   = in_win ? wmapy : mapy;
                u16 map  = in_win ? win_map : bg_map;
                u8 id = fifo_vram(g, map + my * 32 + mapx);
                ftaddr = unsigned_tiles ? 0x8000 + id * 16
                                        : 0x9000 + (i16)((i8)id) * 16;
                fstate = 1;
            } else if (fstate == 1) {
                flo = fifo_vram(g, ftaddr + (in_win ? wtrow : trow) * 2); fstate = 2;
            } else if (fstate == 2) {
                fhi = fifo_vram(g, ftaddr + (in_win ? wtrow : trow) * 2 + 1); fstate = 3;
            }
            if (fstate == 3) {
                if (warmup) { warmup = 0; fstate = 0; }    /* drop the warm-up tile */
                else if (flen <= 8) {
                    for (int p = 0; p < 8; p++)
                        fifo[(fhead + flen + p) & 15] =
                            (u8)((((fhi >> (7 - p)) & 1) << 1) | ((flo >> (7 - p)) & 1));
                    flen += 8; fetch_col++; fstate = 0;
                }
            }
        }
        if (stall > 0) {
            stall--;                                  /* object fetch in progress: no pixel out */
        } else if (flen > 0) {
            if (discard > 0) {                        /* SCX fine-scroll discard */
                fhead = (fhead + 1) & 15; flen--; discard--;
            } else {
                /* Reaching an object stalls the pipeline to fetch it: a flat 6 dots,
                 * plus a once-per-BG-tile alignment cost. This is where the mode-3
                 * object penalty EMERGES (the scanline renderer instead computes it). */
                int st = 0;
                for (int s = 0; s < nsp; s++) {
                    if (sp[s].fetched || sp[s].x >= 168) continue;
                    int left = sp[s].x - 8;
                    if (left > out_x) break;          /* sorted by x: none earlier remain */
                    sp[s].fetched = 1;
                    int bgp = (sp[s].x - 8) + scx, tile = bgp >> 3, considered = 0;
                    for (int k = 0; k < ns; k++) if (seen[k] == tile) { considered = 1; break; }
                    if (!considered) {
                        seen[ns++] = tile;
                        int off = (sp[s].x == 0) ? 0 : (bgp & 7), p = (7 - off) - 2;
                        if (p > 0) st += p;
                    }
                    st += 6;
                    int soff = (left < out_x) ? (out_x - left) : 0;   /* off-screen-left */
                    for (int p = soff; p < 8; p++) {
                        int bit = (sp[s].attr & 0x20) ? p : (7 - p);
                        u8 c = (u8)((((sp[s].hi >> bit) & 1) << 1) | ((sp[s].lo >> bit) & 1));
                        int pos = p - soff;
                        if (c && obj_cn[pos] == 0) {
                            obj_cn[pos] = c;
                            obj_pal[pos] = (sp[s].attr & 0x10) ? g->obp1 : g->obp0;
                            obj_bh[pos] = (sp[s].attr & 0x80) ? 1 : 0;
                        }
                    }
                }
                if (st > 0) { stall = st - 1; }       /* pause; this dot is the first stall dot */
                else {
                    u8 cn = fifo[fhead]; fhead = (fhead + 1) & 15; flen--;
                    u8 shade = (obj_cn[0] && !(obj_bh[0] && cn != 0))
                             ? (u8)((obj_pal[0] >> (obj_cn[0] * 2)) & 3)
                             : (u8)((g->bgp >> (cn * 2)) & 3);
                    out[out_x++] = shade;
                    for (int p = 0; p < 7; p++) {
                        obj_cn[p] = obj_cn[p + 1]; obj_pal[p] = obj_pal[p + 1]; obj_bh[p] = obj_bh[p + 1];
                    }
                    obj_cn[7] = obj_pal[7] = obj_bh[7] = 0;
                }
            }
        }
        dot++;
    }
    return dot;
}
