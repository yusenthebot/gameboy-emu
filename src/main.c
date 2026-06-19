/* main.c - entry point.
 *
 * Three modes:
 *   test  (default): run until serial prints "Passed"/"Failed", exit code 0=pass.
 *   frame (--frames N): run N frames, dump framebuffer to PNG and/or raw.
 *   mooneye (--mooneye): run to the LD B,B (0x40) breakpoint, check the
 *           Fibonacci register signature (B=3 C=5 D=8 E=13 H=21 L=34 = pass).
 *
 * Usage:
 *   gbemu <rom.gb> [max_cycles]
 *   gbemu <rom.gb> --frames N [--png out.png] [--raw out.raw] [--cycles C]
 *   gbemu <rom.gb> --mooneye [--cycles C]
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GB gb;   /* static: embeds a 64KB serial log + framebuffer */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.gb> [max_cycles | --frames N [--png p][--raw p][--cycles C]]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    int frames = 0, mooneye = 0, debug = 0, rewind_test = 0, sav_selftest = 0, force_cgb = 0;
    int wram_selftest = 0, hdma_selftest = 0, apu_activity = 0, fifo_selftest = 0;
    const char *sav_path = NULL;
    const char *png_path = NULL, *raw_path = NULL, *keys = NULL, *rgb_path = NULL;
    const char *load_state = NULL, *save_state = NULL, *audio_raw = NULL;
    u64 max_cycles = 350000000ULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_path = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(argv[i], "--rgb") && i + 1 < argc) rgb_path = argv[++i];
        else if (!strcmp(argv[i], "--mooneye")) mooneye = 1;
        else if (!strcmp(argv[i], "--debug")) debug = 1;
        else if (!strcmp(argv[i], "--rewind-selftest")) rewind_test = 1;
        else if (!strcmp(argv[i], "--sav-selftest")) sav_selftest = 1;
        else if (!strcmp(argv[i], "--sav") && i + 1 < argc) sav_path = argv[++i];
        else if (!strcmp(argv[i], "--cgb")) force_cgb = 1;   /* run as CGB hardware */
        else if (!strcmp(argv[i], "--apu-activity")) apu_activity = 1;
        else if (!strcmp(argv[i], "--fifo-selftest")) fifo_selftest = 1;
        else if (!strcmp(argv[i], "--wram-selftest")) wram_selftest = 1;
        else if (!strcmp(argv[i], "--hdma-selftest")) hdma_selftest = 1;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) load_state = argv[++i];
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc) save_state = argv[++i];
        else if (!strcmp(argv[i], "--audio-raw") && i + 1 < argc) audio_raw = argv[++i];
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) max_cycles = strtoull(argv[++i], NULL, 0);
        else if (argv[i][0] != '-') max_cycles = strtoull(argv[i], NULL, 0);
    }

    /* --keys "frame:btn,frame:btn,..." : set the joypad at given frames.
     * btn = one of right,left,up,down,a,b,select,start,none. Held until next. */
    struct { u32 frame; u8 mask; } kev[32]; int nkev = 0;
    if (keys) {
        char buf[256]; strncpy(buf, keys, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (char *tok = strtok(buf, ","); tok && nkev < 32; tok = strtok(NULL, ",")) {
            char *colon = strchr(tok, ':'); if (!colon) continue;
            *colon = 0; u32 fr = (u32)atoi(tok); const char *b = colon + 1; u8 m = 0;
            if (!strcmp(b, "right")) m = 0x01; else if (!strcmp(b, "left")) m = 0x02;
            else if (!strcmp(b, "up")) m = 0x04; else if (!strcmp(b, "down")) m = 0x08;
            else if (!strcmp(b, "a")) m = 0x10; else if (!strcmp(b, "b")) m = 0x20;
            else if (!strcmp(b, "select")) m = 0x40; else if (!strcmp(b, "start")) m = 0x80;
            kev[nkev].frame = fr; kev[nkev].mask = m; nkev++;
        }
    }

    if (fifo_selftest) {
        /* Pixel-FIFO BG renderer (T-cycle PPU spike): validate it reproduces the
         * background formula across SCX/SCY/line, with no ROM. */
        memset(&gb, 0, sizeof gb);
        gb.lcdc = 0x91; gb.bgp = 0xE4;                 /* LCD on, tiles @8000, BG map @9800 */
        for (int i = 0; i < 0x400; i++) gb.vram[0x1800 + i] = (u8)(i * 7 + 3);   /* tilemap */
        for (int i = 0; i < 0x1000; i++) gb.vram[i] = (u8)(i * 37 + 11);          /* tile data */
        int ok = 1, tested = 0;
        for (int scx = 0; scx < 256 && ok; scx += 13)
        for (int scy = 0; scy < 256 && ok; scy += 31)
        for (int yy = 0; yy < 144 && ok; yy += 47) {
            gb.scx = (u8)scx; gb.scy = (u8)scy;
            u8 fb[160]; fifo_bg_line(&gb, yy, fb);
            for (int x = 0; x < 160; x++) {
                int bx = (x + scx) & 0xFF, by = (yy + scy) & 0xFF;
                u8 id = gb.vram[0x1800 + (by / 8) * 32 + (bx / 8)];
                int ta = id * 16;
                u8 lo = gb.vram[ta + (by % 8) * 2], hi = gb.vram[ta + (by % 8) * 2 + 1];
                u8 cn = (u8)((((hi >> (7 - (bx % 8))) & 1) << 1) | ((lo >> (7 - (bx % 8))) & 1));
                if (fb[x] != (u8)((gb.bgp >> (cn * 2)) & 3)) { ok = 0; break; }
            }
            tested++;
        }
        fprintf(stderr, "RESULT: %s (%d lines)\n", ok ? "PASS" : "FAIL", tested);
        return ok ? 0 : 1;
    }

    memset(&gb, 0, sizeof(gb));
    if (cart_load(&gb, path) != 0) return 2;
    if (force_cgb) gb.cgb = true;        /* CGB hardware runs any cart in CGB mode */
    cpu_init_postboot(&gb);
    if (load_state) {
        if (gb_load_state(&gb, load_state) != 0) {
            fprintf(stderr, "load-state failed: %s\n", load_state);
            return 2;
        }
        fprintf(stderr, "loaded state %s (frame %llu)\n", load_state,
                (unsigned long long)gb.frame_count);
    }

    if (debug) {
        debugger_repl(&gb);
        cart_free(&gb);
        return 0;
    }

    if (sav_selftest) {
        /* Battery save round-trip: write a pattern to cart RAM (+RTC), save it,
         * clear, reload, and verify it persisted bit-for-bit. */
        Cart *c = &gb.cart;
        if (!c->ram || c->ram_size == 0) { fprintf(stderr, "RESULT: FAIL (no cart RAM)\n"); return 1; }
        for (size_t i = 0; i < c->ram_size; i++) c->ram[i] = (u8)(i * 7 + 3);
        for (int i = 0; i < 5; i++) c->rtc[i] = (u8)(i + 1);
        cart_save_battery(&gb, "/tmp/_gbemu_sav_selftest.sav");
        memset(c->ram, 0, c->ram_size);
        memset(c->rtc, 0, sizeof c->rtc);
        int ok = cart_load_battery(&gb, "/tmp/_gbemu_sav_selftest.sav") == 0;
        for (size_t i = 0; ok && i < c->ram_size; i++) if (c->ram[i] != (u8)(i * 7 + 3)) ok = 0;
        if (c->mbc == 3) for (int i = 0; ok && i < 5; i++) if (c->rtc[i] != (u8)(i + 1)) ok = 0;
        fprintf(stderr, "RESULT: %s\n", ok ? "PASS" : "FAIL");
        cart_free(&gb);
        return ok ? 0 : 1;
    }
    if (wram_selftest) {
        /* CGB WRAM banking (SVBK): banks 1-7 each hold distinct data; 0xCxxx is
         * the fixed bank-0 region (not switched). */
        gb.cgb = true;
        int ok = 1;
        for (int b = 1; b <= 7; b++) {
            bus_write(&gb, 0xFF70, b);
            bus_write(&gb, 0xD000, (u8)(0x10 + b));
            bus_write(&gb, 0xDFFF, (u8)(0x80 + b));
        }
        for (int b = 1; b <= 7; b++) {
            bus_write(&gb, 0xFF70, b);
            if (bus_read(&gb, 0xD000) != (u8)(0x10 + b)) ok = 0;
            if (bus_read(&gb, 0xDFFF) != (u8)(0x80 + b)) ok = 0;
        }
        bus_write(&gb, 0xFF70, 1); bus_write(&gb, 0xC000, 0xAB);   /* bank-0 region */
        bus_write(&gb, 0xFF70, 5);
        if (bus_read(&gb, 0xC000) != 0xAB) ok = 0;                 /* unaffected by SVBK */
        if (bus_read(&gb, 0xFF70) != (0x05 | 0xF8)) ok = 0;        /* SVBK read-back */
        fprintf(stderr, "RESULT: %s\n", ok ? "PASS" : "FAIL");
        cart_free(&gb);
        return ok ? 0 : 1;
    }
    if (hdma_selftest) {
        /* CGB VRAM DMA: general-purpose copies all blocks at once; HBlank mode
         * copies one 0x10-byte block per HBlank step. */
        gb.cgb = true;
        int ok = 1;
        bus_write(&gb, 0xFF70, 1);
        for (int i = 0; i < 0x20; i++) bus_write(&gb, 0xD000 + i, (u8)(i ^ 0x5A));
        /* general-purpose: src 0xD000 -> VRAM 0x0000, 2 blocks */
        bus_write(&gb, 0xFF51, 0xD0); bus_write(&gb, 0xFF52, 0x00);
        bus_write(&gb, 0xFF53, 0x00); bus_write(&gb, 0xFF54, 0x00);
        bus_write(&gb, 0xFF55, 0x01);
        for (int i = 0; i < 0x20; i++) if (gb.vram[i] != (u8)(i ^ 0x5A)) ok = 0;
        if (bus_read(&gb, 0xFF55) != 0xFF) ok = 0;                 /* done */
        /* HBlank-driven: src 0xD000 -> VRAM 0x0100, 2 blocks, stepped manually */
        bus_write(&gb, 0xFF53, 0x01); bus_write(&gb, 0xFF54, 0x00);
        bus_write(&gb, 0xFF55, 0x81);
        if (bus_read(&gb, 0xFF55) != 0x01) ok = 0;                 /* active, 1 left */
        hdma_hblank_step(&gb);
        if (bus_read(&gb, 0xFF55) != 0x00) ok = 0;
        hdma_hblank_step(&gb);
        if (bus_read(&gb, 0xFF55) != 0xFF) ok = 0;                 /* done */
        for (int i = 0; i < 0x20; i++) if (gb.vram[0x100 + i] != (u8)(i ^ 0x5A)) ok = 0;
        fprintf(stderr, "RESULT: %s\n", ok ? "PASS" : "FAIL");
        cart_free(&gb);
        return ok ? 0 : 1;
    }
    if (sav_path) cart_load_battery(&gb, sav_path);   /* resume from a battery save */

    if (rewind_test) {
        /* (a) snapshot/restore must round-trip exactly; (b) a rewind to an earlier
         * snapshot then replay must be bit-identical to the first pass. */
        enum { SNAP = 100, T = 200 };
        size_t sz = gb_snapshot_size(&gb);
        u8 *snap = malloc(sz);

        while (gb.frame_count < (u64)SNAP) cpu_step(&gb);
        gb_snapshot(&gb, snap);
        u8 fb_snap[160 * 144];
        memcpy(fb_snap, gb.fb, sizeof fb_snap);

        gb_restore(&gb, snap);                          /* immediate round-trip */
        int rt = (gb.frame_count == SNAP) && memcmp(gb.fb, fb_snap, sizeof fb_snap) == 0;

        while (gb.frame_count < (u64)T) cpu_step(&gb);  /* first pass to T */
        u8 fb1[160 * 144];
        memcpy(fb1, gb.fb, sizeof fb1);

        gb_restore(&gb, snap);                          /* rewind to SNAP */
        while (gb.frame_count < (u64)T) cpu_step(&gb);  /* replay to T */
        int replay = memcmp(fb1, gb.fb, sizeof fb1) == 0;

        fprintf(stderr, "rewind: round-trip %s, replay %s\n",
                rt ? "OK" : "FAIL", replay ? "OK" : "FAIL");
        fprintf(stderr, "RESULT: %s\n", (rt && replay) ? "PASS" : "FAIL");
        free(snap);
        cart_free(&gb);
        return (rt && replay) ? 0 : 1;
    }

    if (mooneye) {
        /* Mooneye signals completion with LD B,B (0x40); registers hold the
         * Fibonacci sequence on success. */
        int result = 1;
        while (gb.cycles < max_cycles) {
            /* Completion breakpoint: LD B,B (0x40, Mooneye) or the illegal
             * opcode 0xED (Wilbert Pol's fork). Registers hold the Fibonacci
             * sequence on success. */
            u8 op = bus_read(&gb, gb.pc);
            if (!gb.halted && (op == 0x40 || op == 0xED)) {
                result = (gb.b == 3 && gb.c == 5 && gb.d == 8 &&
                          gb.e == 13 && gb.h == 21 && gb.l == 34) ? 0 : 1;
                break;
            }
            cpu_step(&gb);
        }
        fprintf(stderr, "mooneye regs B=%d C=%d D=%d E=%d H=%d L=%d cycles=%llu\n",
                gb.b, gb.c, gb.d, gb.e, gb.h, gb.l, (unsigned long long)gb.cycles);
        fprintf(stderr, "RESULT: %s\n", result == 0 ? "PASS" : "FAIL");
        cart_free(&gb);
        return result;
    }

    if (apu_activity) {
        /* Gambatte outaudio: run a fixed 15 LCD frames (1053360 clock cycles, the
         * suite's exit condition — LCD-independent), measuring whether the APU
         * output varies over the final frame (audio1) or is constant (audio0). */
        const u64 FRAME_CYC = 70224, TOTAL = 15 * 70224;   /* crystal clocks (gambatte's exit) */
        int reset_done = 0;
        while (gb.sys_cycles < TOTAL) {
            if (!reset_done && gb.sys_cycles >= TOTAL - FRAME_CYC) { apu_activity_reset(); reset_done = 1; }
            cpu_step(&gb);
        }
        printf("RESULT: %s\n", apu_activity_varied() ? "audio1" : "audio0");
        cart_free(&gb);
        return 0;
    }

    if (frames > 0) {
        /* Frame-dump mode (with optional scripted input). */
        int ke = 0;
        FILE *af = audio_raw ? fopen(audio_raw, "wb") : NULL;
        i16 sbuf[2048];
        u64 lastf = gb.frame_count;
        while (gb.cycles < max_cycles && gb.frame_count < (u64)frames) {
            while (ke < nkev && gb.frame_count >= kev[ke].frame) gb.buttons = kev[ke++].mask;
            cpu_step(&gb);
            if (af && gb.frame_count != lastf) {           /* drain audio each frame */
                lastf = gb.frame_count;
                int n = apu_drain_samples(&gb, sbuf, 1024);
                if (n) fwrite(sbuf, sizeof(i16), (size_t)n * 2, af);
            }
        }
        if (af) {
            int n = apu_drain_samples(&gb, sbuf, 1024);
            if (n) fwrite(sbuf, sizeof(i16), (size_t)n * 2, af);
            fclose(af);
            fprintf(stderr, "wrote audio %s\n", audio_raw);
        }
        fprintf(stderr, "frames=%llu cycles=%llu\n",
                (unsigned long long)gb.frame_count, (unsigned long long)gb.cycles);
        if (save_state) {
            if (gb_save_state(&gb, save_state) == 0)
                fprintf(stderr, "saved state %s\n", save_state);
            else
                fprintf(stderr, "save-state failed: %s\n", save_state);
        }
        if (png_path) {
            if (png_write_gray(png_path, 160, 144, gb.fb) == 0)
                fprintf(stderr, "wrote %s\n", png_path);
            else
                fprintf(stderr, "PNG write failed: %s\n", png_path);
        }
        if (raw_path) {
            FILE *rf = fopen(raw_path, "wb");
            if (rf) { fwrite(gb.fb, 1, sizeof(gb.fb), rf); fclose(rf);
                      fprintf(stderr, "wrote %s\n", raw_path); }
        }
        if (rgb_path) {                       /* CGB color framebuffer (raw 0x00RRGGBB) */
            FILE *rf = fopen(rgb_path, "wb");
            if (rf) { fwrite(gb.fb_rgb, sizeof(u32), 160 * 144, rf); fclose(rf);
                      fprintf(stderr, "wrote %s\n", rgb_path); }
        }
        cart_free(&gb);
        return (gb.frame_count >= (u64)frames) ? 0 : 1;
    }

    /* Serial test-harness mode. */
    int result = -1;
    size_t last_len = 0;
    while (gb.cycles < max_cycles) {
        cpu_step(&gb);
        if (gb.serial_len != last_len) {
            last_len = gb.serial_len;
            if (strstr(gb.serial_log, "Passed")) { result = 0; break; }
            if (strstr(gb.serial_log, "Failed")) { result = 1; break; }
        }
    }
    fprintf(stderr, "\n--- run ended: cycles=%llu serial_len=%zu ---\n",
            (unsigned long long)gb.cycles, gb.serial_len);
    if (result == 0)      fprintf(stderr, "RESULT: PASS\n");
    else if (result == 1) fprintf(stderr, "RESULT: FAIL\n");
    else                  fprintf(stderr, "RESULT: TIMEOUT/UNKNOWN\n");

    cart_free(&gb);
    return (result == 0) ? 0 : 1;
}
