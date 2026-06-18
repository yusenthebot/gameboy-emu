/* main.c - headless test-harness entry point.
 *
 * Runs a ROM until its serial output contains "Passed" or "Failed"
 * (Blargg convention) or a cycle budget is exhausted. Exit code 0 = pass.
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Static: the GB struct embeds a 64KB serial log; keep it off the stack. */
static GB gb;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <rom.gb> [max_cycles]\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];
    /* ~70224 cycles/frame * 60 fps * 80s default budget. */
    u64 max_cycles = (argc >= 3) ? strtoull(argv[2], NULL, 0) : 350000000ULL;

    memset(&gb, 0, sizeof(gb));
    if (cart_load(&gb, path) != 0) return 2;
    cpu_init_postboot(&gb);

    int result = -1;             /* -1 unknown, 0 pass, 1 fail */
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
