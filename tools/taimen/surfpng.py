#!/usr/bin/env python3
"""PNGs of a surface fetched with `phonectl.py surf ID OUT.raw` (xnu 0369).

  surfpng.py OUT.raw [PREFIX]    -> PREFIX-rgb.png (and for 420f/420v: PREFIX-y.png, the luma,
                                    and PREFIX-c.png, the chroma plane drawn at mid grey)

PREFIX defaults to OUT. The geometry comes from OUT.raw.txt (the kernel's "surfs" line).
Bi-planar 4:2:0 is converted with BT.601 (420f full range, 420v video range): enough to see
which plane is wrong, not a colour reference.
"""
import re
import struct
import sys
import zlib


def png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))
    print(f'{path}: {w}x{h}')


def clamp(v):
    return 0 if v < 0 else 255 if v > 255 else int(v)


def main():
    raw = sys.argv[1]
    prefix = sys.argv[2] if len(sys.argv) > 2 else raw
    desc = open(raw + '.txt').read().strip()
    data = open(raw, 'rb').read()
    m = re.match(r'(\d+) (\S+) (\d+)x(\d+) fmt (\S{4}) bpe (\d+) bpr (\d+) offset (0x[0-9a-f]+)', desc)
    if not m:
        sys.exit(f'{raw}.txt: not a surfs line: {desc}')
    w, h, fmt, bpr, off = int(m[3]), int(m[4]), m[5], int(m[7]), int(m[8], 16)
    planes = [(int(a), int(b), int(c), int(d, 16)) for a, b, c, d in
              re.findall(r'\[(\d+)x(\d+) bpe \d+ bpr (\d+) offset (0x[0-9a-f]+)', desc)]
    if fmt in ('420f', '420v') and len(planes) == 2:
        (yw, yh, ybpr, yoff), (cw, ch, cbpr, coff) = planes
        full = fmt == '420f'
        gray, cimg, rgb = bytearray(yw * yh * 3), bytearray(cw * ch * 3), bytearray(yw * yh * 3)
        for y in range(yh):
            yrow = data[yoff + y * ybpr:yoff + y * ybpr + yw]
            crow = data[coff + (y // 2) * cbpr:coff + (y // 2) * cbpr + 2 * cw]
            for x in range(yw):
                Y = yrow[x]
                cb, cr = crow[2 * (x // 2)] - 128, crow[2 * (x // 2) + 1] - 128
                if not full:
                    Y, cb, cr = (Y - 16) * 255 / 219, cb * 255 / 224, cr * 255 / 224
                i = 3 * (y * yw + x)
                gray[i:i + 3] = bytes((yrow[x],) * 3)
                rgb[i:i + 3] = bytes((clamp(Y + 1.402 * cr), clamp(Y - 0.344 * cb - 0.714 * cr), clamp(Y + 1.772 * cb)))
        for y in range(ch):
            crow = data[coff + y * cbpr:coff + y * cbpr + 2 * cw]
            for x in range(cw):
                cb, cr = crow[2 * x] - 128, crow[2 * x + 1] - 128
                i = 3 * (y * cw + x)
                cimg[i:i + 3] = bytes((clamp(128 + 1.402 * cr), clamp(128 - 0.344 * cb - 0.714 * cr), clamp(128 + 1.772 * cb)))
        png(prefix + '-rgb.png', yw, yh, bytes(rgb))
        png(prefix + '-y.png', yw, yh, bytes(gray))
        png(prefix + '-c.png', cw, ch, bytes(cimg))
    elif fmt in ('BGRA', 'RGBA'):
        rgb = bytearray(w * h * 3)
        for y in range(h):
            row = data[off + y * bpr:off + y * bpr + 4 * w]
            r, b = (2, 0) if fmt == 'BGRA' else (0, 2)
            rgb[3 * w * y + 0:3 * w * (y + 1):3] = row[r::4]
            rgb[3 * w * y + 1:3 * w * (y + 1):3] = row[1::4]
            rgb[3 * w * y + 2:3 * w * (y + 1):3] = row[b::4]
        png(prefix + '-rgb.png', w, h, bytes(rgb))
    elif fmt in ('L008', 'A008'):
        rgb = bytearray(w * h * 3)
        for y in range(h):
            row = data[off + y * bpr:off + y * bpr + w]
            for c in range(3):
                rgb[3 * w * y + c:3 * w * (y + 1):3] = row
        png(prefix + '-rgb.png', w, h, bytes(rgb))
    else:
        sys.exit(f'{raw}: format {fmt} with {len(planes)} planes: not drawn')


main()
