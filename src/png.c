/* png.c - minimal grayscale PNG writer (no external deps).
 * Emits 8-bit grayscale using uncompressed (stored) zlib blocks, so no
 * compression library is needed. Used for visual frame dumps.
 */
#include "gb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static u32 crc_table[256];
static int crc_ready = 0;

static void crc_init(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

static u32 crc32_buf(const u8 *p, size_t n) {
    if (!crc_ready) crc_init();
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u32 adler32_buf(const u8 *p, size_t n) {
    u32 a = 1, b = 0;
    for (size_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static void put_be32(FILE *f, u32 v) {
    fputc((v >> 24) & 0xFF, f); fputc((v >> 16) & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);  fputc(v & 0xFF, f);
}

static void write_chunk(FILE *f, const char *type, const u8 *data, u32 len) {
    put_be32(f, len);
    u8 *tmp = malloc(len + 4);
    memcpy(tmp, type, 4);
    if (len) memcpy(tmp + 4, data, len);
    fwrite(tmp, 1, len + 4, f);
    put_be32(f, crc32_buf(tmp, len + 4));
    free(tmp);
}

/* map shade index 0..3 -> grayscale (0 = white, 3 = black) */
static const u8 SHADE_GRAY[4] = {0xFF, 0xAA, 0x55, 0x00};

int png_write_gray(const char *path, int w, int h, const u8 *indices) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    u8 ihdr[13];
    ihdr[0] = (w >> 24) & 0xFF; ihdr[1] = (w >> 16) & 0xFF;
    ihdr[2] = (w >> 8) & 0xFF;  ihdr[3] = w & 0xFF;
    ihdr[4] = (h >> 24) & 0xFF; ihdr[5] = (h >> 16) & 0xFF;
    ihdr[6] = (h >> 8) & 0xFF;  ihdr[7] = h & 0xFF;
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 0;   /* color type: grayscale */
    ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    write_chunk(f, "IHDR", ihdr, 13);

    /* raw image data: per row a filter byte (0) + w gray samples */
    size_t raw_len = (size_t)h * (w + 1);
    u8 *raw = malloc(raw_len);
    size_t o = 0;
    for (int y = 0; y < h; y++) {
        raw[o++] = 0;
        for (int x = 0; x < w; x++)
            raw[o++] = SHADE_GRAY[indices[y * w + x] & 3];
    }

    /* zlib stream: header + stored DEFLATE blocks + adler32 */
    size_t cap = raw_len + (raw_len / 65535 + 1) * 5 + 16;
    u8 *z = malloc(cap);
    size_t zo = 0;
    z[zo++] = 0x78; z[zo++] = 0x01;
    size_t pos = 0;
    while (pos < raw_len) {
        size_t block = raw_len - pos;
        if (block > 65535) block = 65535;
        int final = (pos + block >= raw_len) ? 1 : 0;
        z[zo++] = (u8)final;                 /* BFINAL, BTYPE=00 (stored) */
        z[zo++] = block & 0xFF; z[zo++] = (block >> 8) & 0xFF;
        u16 nlen = ~(u16)block;
        z[zo++] = nlen & 0xFF; z[zo++] = (nlen >> 8) & 0xFF;
        memcpy(z + zo, raw + pos, block);
        zo += block; pos += block;
    }
    u32 ad = adler32_buf(raw, raw_len);
    z[zo++] = (ad >> 24) & 0xFF; z[zo++] = (ad >> 16) & 0xFF;
    z[zo++] = (ad >> 8) & 0xFF;  z[zo++] = ad & 0xFF;

    write_chunk(f, "IDAT", z, (u32)zo);
    write_chunk(f, "IEND", NULL, 0);

    free(raw); free(z);
    fclose(f);
    return 0;
}
