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

    u8 fifo[16]; int fhead = 0, flen = 0;
    int fetch_col = 0, fstate = 0, ftimer = 0;
    u16 ftaddr = 0; u8 flo = 0, fhi = 0;
    int warmup = 1, discard = scx & 7, in_win = 0;
    int out_x = 0, dot = 0;

    while (out_x < 160) {
        /* window trigger: the next visible pixel is inside the window -> the
         * fetcher restarts on the window map (FIFO cleared). */
        if (win_on && !in_win && out_x + 7 >= wx) {
            in_win = 1; flen = 0; fhead = 0; fstate = 0; ftimer = 0;
            fetch_col = 0; warmup = 1;
            discard = (wx < 7) ? (7 - wx) : 0;   /* window starting left of x=0 */
        }
        if (++ftimer >= 2) {
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
        if (flen > 0) {
            u8 cn = fifo[fhead]; fhead = (fhead + 1) & 15; flen--;
            if (discard > 0) discard--;
            else out[out_x++] = (u8)((g->bgp >> (cn * 2)) & 3);
        }
        dot++;
    }
    return dot;
}
