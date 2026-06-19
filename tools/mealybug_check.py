#!/usr/bin/env python3
"""Mealybug-tearoom verifier: decode a grayscale reference PNG (no PIL) and the
emulator's grayscale PNG dump, quantize both to the 4 DMG shades, compare exact.
Usage: mealybug_check.py <reference.png> <emu_dump.png>  -> exit 0 pass / 1 fail."""
import sys, struct, zlib


def decode_gray_png(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not a png'
    w = h = bitd = ctype = None
    idat = b''
    i = 8
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]
        typ = d[i+4:i+8]
        body = d[i+8:i+8+ln]
        if typ == b'IHDR':
            w, h, bitd, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        i += 12 + ln
    assert ctype == 0, f'colortype {ctype} not grayscale'
    raw = zlib.decompress(idat)
    # bytes per scanline (grayscale, bitdepth bitd)
    bpp = 1  # samples per pixel for grayscale
    stride = (w * bitd + 7) // 8
    out = []
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        # unfilter (bpp in bytes for sub/paeth = ceil(bitd/8) but for <8bpp it's 1)
        fb = max(1, bpp * bitd // 8)
        for x in range(stride):
            a = line[x-fb] if x >= fb else 0
            b = prev[x]
            c = prev[x-fb] if x >= fb else 0
            if ft == 1: line[x] = (line[x] + a) & 255
            elif ft == 2: line[x] = (line[x] + b) & 255
            elif ft == 3: line[x] = (line[x] + (a + b)//2) & 255
            elif ft == 4:
                pp = a + b - c; pa = abs(pp-a); pb = abs(pp-b); pc = abs(pp-c)
                pr = a if (pa<=pb and pa<=pc) else (b if pb<=pc else c)
                line[x] = (line[x] + pr) & 255
        prev = line
        # unpack bits to pixel samples, scale to 0..255
        row = []
        maxv = (1 << bitd) - 1
        if bitd == 8:
            row = [line[x] for x in range(w)]
        else:
            for x in range(w):
                bitpos = x * bitd
                byte = line[bitpos // 8]
                shift = 8 - bitd - (bitpos % 8)
                v = (byte >> shift) & maxv
                row.append(v * 255 // maxv)
        out.append(row)
    return w, h, out


def quant(v):           # gray 0..255 -> DMG shade 0(light)..3(dark) by nearest of 4
    return min(range(4), key=lambda s: abs(v - (255 - s*85)))


def compare(ref_path, emu_path):
    rw, rh, ref = decode_gray_png(ref_path)
    ew, eh, emu = decode_gray_png(emu_path)
    if (rw, rh) != (ew, eh):
        return False
    for y in range(rh):
        for x in range(rw):
            if quant(ref[y][x]) != quant(emu[y][x]):
                return False
    return True


if __name__ == '__main__':
    try:
        ok = compare(sys.argv[1], sys.argv[2])
    except Exception as e:
        sys.stderr.write(f'error: {e}\n'); sys.exit(2)
    sys.exit(0 if ok else 1)
