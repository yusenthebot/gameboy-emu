#!/usr/bin/env python3
"""Compare a CGB color framebuffer dump (raw 0x00RRGGBB, 160x144) against a
reference PNG (handles paletted / grayscale / RGB). Exit 0 if identical."""
import sys, struct, zlib

def load_png_rgb(path):
    f = open(path, 'rb').read()
    assert f[:8] == b'\x89PNG\r\n\x1a\n', "not a PNG"
    pos, w, h, bd, ct, plte, idat = 8, 0, 0, 0, 0, b'', b''
    while pos < len(f):
        ln = struct.unpack('>I', f[pos:pos+4])[0]
        typ = f[pos+4:pos+8]; data = f[pos+8:pos+8+ln]
        if typ == b'IHDR': w, h, bd, ct = struct.unpack('>IIBB', data[:10])
        elif typ == b'PLTE': plte = data
        elif typ == b'IDAT': idat += data
        pos += 12 + ln
    raw = zlib.decompress(idat)
    stride = (w * bd * (3 if ct == 2 else 1) + 7) // 8
    bpp = max(1, (bd * (3 if ct == 2 else 1)) // 8)
    def paeth(a, b, c):
        p = a + b - c; pa, pb, pc = abs(p-a), abs(p-b), abs(p-c)
        return a if pa <= pb and pa <= pc else (b if pb <= pc else c)
    rows, prev, p = [], bytearray(stride), 0
    for _ in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for i in range(stride):
            a = line[i-bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i-bpp] if i >= bpp else 0
            if ft == 1: line[i] = (line[i]+a) & 0xFF
            elif ft == 2: line[i] = (line[i]+b) & 0xFF
            elif ft == 3: line[i] = (line[i]+((a+b)>>1)) & 0xFF
            elif ft == 4: line[i] = (line[i]+paeth(a, b, c)) & 0xFF
        prev = line
        rows.append(bytes(line))
    rgb = []
    for line in rows:
        for x in range(w):
            if ct == 3:                                  # paletted
                byte = line[(x*bd)//8]
                if bd == 4: idx = (byte >> 4) if x % 2 == 0 else (byte & 0xF)
                elif bd == 8: idx = line[x]
                elif bd == 2: idx = (byte >> (6-2*(x % 4))) & 3
                else: idx = (byte >> (7-(x % 8))) & 1
                r, g, b = plte[idx*3], plte[idx*3+1], plte[idx*3+2]
            elif ct == 2:                                # RGB
                r, g, b = line[x*3], line[x*3+1], line[x*3+2]
            else:                                        # grayscale
                v = line[x]; r = g = b = v
            rgb.append((r << 16) | (g << 8) | b)
    return w, h, rgb

w, h, ref = load_png_rgb(sys.argv[1])
d = open(sys.argv[2], 'rb').read()
mine = struct.unpack('<%dI' % (len(d)//4), d)
mism = sum(1 for a, b in zip(ref, mine) if (a & 0xFFFFFF) != (b & 0xFFFFFF))
print(f"mismatches: {mism} / {len(ref)}")
sys.exit(0 if mism == 0 else 1)
