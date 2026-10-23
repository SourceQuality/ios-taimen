#!/usr/bin/env python3
"""Turn the kernel's screenshot lines (xnu 0205: "SHOT name w h", base64 "SHOT64", "SHOT END
name") in a console log into PNG files.

  shots.py LOG.txt OUTDIR      -> OUTDIR/name.png for every complete shot

Incomplete shots (the log ended or wrapped inside one) are skipped and reported.
"""
import base64, os, re, struct, sys, zlib


def png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def main():
    log, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    cur, data = None, []
    for line in open(log, encoding='utf-8', errors='replace'):
        line = line.rstrip('\n')
        m = re.match(r'SHOT (\S+) (\d+) (\d+)$', line)
        if m:
            if cur:
                print(f'{cur[0]}: incomplete, skipped')
            cur, data = (m.group(1), int(m.group(2)), int(m.group(3))), []
        elif line.startswith('SHOT64 ') and cur:
            data.append(line[7:].strip())
        elif line.startswith('SHOT END ') and cur:
            name, w, h = cur
            rgb = base64.b64decode(''.join(data))
            if len(rgb) != w * h * 3:
                print(f'{name}: {len(rgb)} bytes, expected {w * h * 3}, skipped')
            else:
                png(os.path.join(out, name + '.png'), w, h, rgb)
                print(f'{out}/{name}.png: {w}x{h}')
            cur = None
    if cur:
        print(f'{cur[0]}: incomplete, skipped')


main()
