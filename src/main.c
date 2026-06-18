/* main.c - entry point.
 *
 * Two modes:
 *   test  (default): run until serial prints "Passed"/"Failed", exit code 0=pass.
 *   frame (--frames N): run N frames, dump framebuffer to PNG and/or raw.
 *
 * Usage:
 *   gbemu <rom.gb> [max_cycles]
 *   gbemu <rom.gb> --frames N [--png out.png] [--raw out.raw] [--cycles C]
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
    int frames = 0;
    const char *png_path = NULL, *raw_path = NULL;
    u64 max_cycles = 350000000ULL;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png_path = argv[++i];
        else if (!strcmp(argv[i], "--raw") && i + 1 < argc) raw_path = argv[++i];
        else if (!strcmp(argv[i], "--cycles") && i + 1 < argc) max_cycles = strtoull(argv[++i], NULL, 0);
        else if (argv[i][0] != '-') max_cycles = strtoull(argv[i], NULL, 0);
    }

    memset(&gb, 0, sizeof(gb));
    if (cart_load(&gb, path) != 0) return 2;
    cpu_init_postboot(&gb);

    if (frames > 0) {
        /* Frame-dump mode. */
        while (gb.cycles < max_cycles && gb.frame_count < (u64)frames)
            cpu_step(&gb);
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
