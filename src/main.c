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
    int frames = 0, mooneye = 0, debug = 0, rewind_test = 0;
    const char *png_path = NULL, *raw_path = NULL, *keys = NULL;
    const char *load_state = NULL, *save_state = NULL, *audio_raw = NULL;
    u64 max_cycles = 350000000ULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_path = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(argv[i], "--mooneye")) mooneye = 1;
        else if (!strcmp(argv[i], "--debug")) debug = 1;
        else if (!strcmp(argv[i], "--rewind-selftest")) rewind_test = 1;
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

    memset(&gb, 0, sizeof(gb));
    if (cart_load(&gb, path) != 0) return 2;
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
