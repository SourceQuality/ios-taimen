#!/usr/bin/env python3
"""Expand an Android sparse image into a raw one (the reverse of tools/img2simg.py).

  simg2img.py IN.simg OUT.img

DONT_CARE chunks and zero FILLs become holes in OUT (a sparse file), so a 16 GiB root image
with 12 GB used takes 12 GB.
"""
import struct, sys

MAGIC = 0xed26ff3a
RAW, FILL, DONT_CARE, CRC = 0xcac1, 0xcac2, 0xcac3, 0xcac4


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f, open(dst, 'wb') as o:
        magic, major, minor, hdr_sz, chunk_hdr_sz, blk, total_blks, total_chunks, _ = \
            struct.unpack('<IHHHHIIII', f.read(28))
        if magic != MAGIC:
            sys.exit('not a sparse image')
        f.seek(hdr_sz)
        for n in range(total_chunks):
            ctype, _, nblk, total = struct.unpack('<HHII', f.read(12))
            f.seek(chunk_hdr_sz - 12, 1)
            size = nblk * blk
            if ctype == RAW:
                left = size
                while left:
                    buf = f.read(min(left, 64 << 20))
                    o.write(buf)
                    left -= len(buf)
            elif ctype == FILL:
                val = f.read(4)
                if val == b'\0\0\0\0':
                    o.seek(size, 1)
                else:
                    o.write(val * (size // 4))
            elif ctype == DONT_CARE:
                o.seek(size, 1)
            elif ctype == CRC:
                f.read(4)
            else:
                sys.exit(f'chunk {n}: unknown type {ctype:#x}')
        o.truncate(total_blks * blk)
    print(f'{dst}: {total_blks * blk} bytes from {total_chunks} chunks')


main()
