#!/usr/bin/env python3
"""Generic PNG-vs-emulator-frame comparator for DMG PPU tests (scribbltests,
turtle-tests, etc.). Decodes any reference PNG (grayscale / RGB / RGBA /
paletted, bitdepth 1/2/4/8) and the emulator's grayscale --png dump, maps both
to the 4 DMG shades by quantizing luminance into 4 fixed bins, and compares
exactly. Usage: pngcmp.py <reference.png> <emu_dump.png>  -> exit 0 if identical."""
import sys, struct, zlib


def _decode(path):
    d = open(path, 'rb').read()
    assert d[:8] == b'\x89PNG\r\n\x1a\n', f'{path}: not a PNG'
    w = h = bitd = ctype = None
    plte = None
    idat = b''
    i = 8
    while i < len(d):
        ln = struct.unpack('>I', d[i:i+4])[0]
        typ = d[i+4:i+8]
        body = d[i+8:i+8+ln]
        if typ == b'IHDR':
            w, h, bitd, ctype = struct.unpack('>IIBB', body[:10])
        elif typ == b'PLTE':
            plte = [(body[j], body[j+1], body[j+2]) for j in range(0, len(body), 3)]
        elif typ == b'IDAT':
            idat += body
        elif typ == b'IEND':
            break
        i += 12 + ln
    # samples per pixel
    spp = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = (w * spp * bitd + 7) // 8
    bpp = max(1, spp * bitd // 8)
    prev = bytearray(stride)
    p = 0
    rows = []
    for _y in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        for x in range(stride):
            a = line[x-bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x-bpp] if x >= bpp else 0
            if ft == 1: line[x] = (line[x] + a) & 255
            elif ft == 2: line[x] = (line[x] + b) & 255
            elif ft == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif ft == 4:
                pp = a + b - c; pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else (b if pb <= pc else c))) & 255
        prev = line
        # extract per-pixel samples -> luminance 0..255
        maxv = (1 << bitd) - 1
        row = []
        for x in range(w):
            if ctype in (0, 4):                       # gray / gray+alpha
                if bitd == 8:
                    v = line[x * spp]
                else:
                    bit = x * spp * bitd
                    byte = line[bit // 8]; sh = 8 - bitd - (bit % 8)
                    v = ((byte >> sh) & maxv) * 255 // maxv
                row.append(v)
            elif ctype == 3:                          # paletted
                bit = x * bitd
                byte = line[bit // 8]; sh = 8 - bitd - (bit % 8)
                r, g, b = plte[(byte >> sh) & maxv]
                row.append((r * 299 + g * 587 + b * 114) // 1000)
            else:                                     # RGB / RGBA (bitdepth 8)
                o = x * spp
                r, g, b = line[o], line[o+1], line[o+2]
                row.append((r * 299 + g * 587 + b * 114) // 1000)
        rows.append(row)
    return w, h, rows


def _shade(lum):                  # luminance -> 0..3 (4 fixed bins)
    return 3 - min(3, lum * 4 // 256)


def compare(ref_path, emu_path):
    rw, rh, ref = _decode(ref_path)
    ew, eh, emu = _decode(emu_path)
    if (rw, rh) != (ew, eh):
        return False
    for y in range(rh):
        for x in range(rw):
            if _shade(ref[y][x]) != _shade(emu[y][x]):
                return False
    return True


if __name__ == '__main__':
    try:
        ok = compare(sys.argv[1], sys.argv[2])
    except Exception as e:
        sys.stderr.write(f'error: {e}\n'); sys.exit(2)
    sys.exit(0 if ok else 1)
