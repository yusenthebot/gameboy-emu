#!/usr/bin/env python3
"""Decode a reference PNG and pixel-diff it against an emulator raw dump.

The emulator dumps a 160x144 framebuffer as one byte/pixel, value 0..3
(0 = lightest shade, 3 = darkest). The reference is a 2-bit grayscale PNG.
We decode it to the same shade-index space and compare exactly.

Usage: imgcmp.py <reference.png> <emu.raw> [--out diff.png]
Exit 0 on an exact match.
"""
import sys
import struct
import zlib

W, H = 160, 144


def decode_png_indices(path):
    """Return (w, h, [shade_index 0..3 per pixel]) for a grayscale PNG."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos = 8
    width = height = bitdepth = colortype = None
    idat = b""
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length  # length + type + data + crc
        if ctype == b"IHDR":
            width, height, bitdepth, colortype = struct.unpack(">IIBB", body[:10])
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    assert colortype == 0, f"expected grayscale, got colortype {colortype}"
    raw = zlib.decompress(idat)

    # channels = 1 (grayscale). bytes per pixel for filtering: ceil(bpp/8) >= 1.
    bits_per_pixel = bitdepth
    filt_bpp = max(1, bits_per_pixel // 8)
    row_bytes = (width * bits_per_pixel + 7) // 8

    # Reconstruct scanlines (undo PNG filters).
    out_rows = []
    prev = bytearray(row_bytes)
    p = 0
    for _y in range(height):
        ft = raw[p]; p += 1
        cur = bytearray(raw[p:p + row_bytes]); p += row_bytes
        for i in range(row_bytes):
            a = cur[i - filt_bpp] if i >= filt_bpp else 0
            b = prev[i]
            c = prev[i - filt_bpp] if i >= filt_bpp else 0
            if ft == 0:
                pass
            elif ft == 1:
                cur[i] = (cur[i] + a) & 0xFF
            elif ft == 2:
                cur[i] = (cur[i] + b) & 0xFF
            elif ft == 3:
                cur[i] = (cur[i] + ((a + b) >> 1)) & 0xFF
            elif ft == 4:
                pa = abs(b - c); pb = abs(a - c); pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pr) & 0xFF
            else:
                raise ValueError(f"bad filter {ft}")
        out_rows.append(bytes(cur))
        prev = cur

    # Unpack sub-byte samples -> luminance -> shade index (0 light .. 3 dark).
    maxval = (1 << bitdepth) - 1
    indices = []
    for row in out_rows:
        samples = []
        for byte in row:
            if bitdepth == 8:
                samples.append(byte)
            else:
                for shift in range(8 - bitdepth, -1, -bitdepth):
                    samples.append((byte >> shift) & maxval)
        samples = samples[:width]
        for s in samples:
            lum = s * 255 // maxval          # 0=black .. 255=white
            level = round(lum / 85)          # 0..3, 3 = white
            indices.append(3 - level)        # shade index: 0 light, 3 dark
    return width, height, indices


def main():
    if len(sys.argv) < 3:
        print("usage: imgcmp.py <reference.png> <emu.raw> [--out diff.png]")
        return 2
    ref_path, raw_path = sys.argv[1], sys.argv[2]
    w, h, ref = decode_png_indices(ref_path)
    if (w, h) != (W, H):
        print(f"reference size {w}x{h} != {W}x{H}")
        return 2
    emu = list(open(raw_path, "rb").read())
    if len(emu) != W * H:
        print(f"raw size {len(emu)} != {W*H}")
        return 2

    diffs = [i for i in range(W * H) if (emu[i] & 3) != (ref[i] & 3)]
    print(f"mismatches: {len(diffs)} / {W*H}")
    if diffs:
        for i in diffs[:8]:
            print(f"  ({i % W},{i // W}) emu={emu[i] & 3} ref={ref[i] & 3}")
        if "--out" in sys.argv:
            out = sys.argv[sys.argv.index("--out") + 1]
            buf = bytearray()
            for i in range(W * H):
                buf.append(0 if (emu[i] & 3) == (ref[i] & 3) else 1)
            _write_diff_png(out, buf)
            print(f"  diff map -> {out}")
    return 0 if not diffs else 1


def _write_diff_png(path, mask):
    """White where match, red-ish (dark) where mismatch; 8-bit grayscale."""
    raw = bytearray()
    for y in range(H):
        raw.append(0)
        for x in range(W):
            raw.append(0 if mask[y * W + x] else 255)
    comp = zlib.compress(bytes(raw))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        ihdr = struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0)
        _chunk(f, b"IHDR", ihdr)
        _chunk(f, b"IDAT", comp)
        _chunk(f, b"IEND", b"")


def _chunk(f, ctype, body):
    f.write(struct.pack(">I", len(body)))
    f.write(ctype + body)
    f.write(struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))


if __name__ == "__main__":
    sys.exit(main())
