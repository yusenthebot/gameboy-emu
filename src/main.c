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
    int frames = 0, mooneye = 0;
    const char *png_path = NULL, *raw_path = NULL, *keys = NULL;
    u64 max_cycles = 350000000ULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_path = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(argv[i], "--mooneye")) mooneye = 1;
        else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
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
        while (gb.cycles < max_cycles && gb.frame_count < (u64)frames) {
            while (ke < nkev && gb.frame_count >= kev[ke].frame) gb.buttons = kev[ke++].mask;
            cpu_step(&gb);
        }
        fprintf(stderr, "frames=%llu cycles=%llu\n",
                (unsigned long long)gb.frame_count, (unsigned long long)gb.cycles);
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
