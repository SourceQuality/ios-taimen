#!/usr/bin/env python3
"""Symbolicate kernel addresses (unslid, as in the sweet panic lines) against a kernel Mach-O.

  symbolicate.py KERNEL ADDR...        e.g. the "SWEET PANIC unslid bt" addresses
"""
import bisect, struct, sys

def symbols(path):
    b = open(path, 'rb').read()
    ncmds = struct.unpack_from('<I', b, 16)[0]
    off, syms = 32, []
    for _ in range(ncmds):
        cmd, csz = struct.unpack_from('<2I', b, off)
        if cmd == 0x2:  # LC_SYMTAB
            symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', b, off + 8)
            for i in range(nsyms):
                n_strx, n_type, n_sect, n_desc, n_value = struct.unpack_from('<IBBHQ', b, symoff + 16 * i)
                if n_type & 0x0e == 0x0e and n_value:
                    end = b.index(b'\0', stroff + n_strx)
                    syms.append((n_value, b[stroff + n_strx:end].decode(errors='replace')))
        off += csz
    syms.sort()
    return syms

if __name__ == '__main__':
    syms = symbols(sys.argv[1])
    addrs = [s[0] for s in syms]
    for a in sys.argv[2:]:
        v = int(a, 16)
        i = bisect.bisect_right(addrs, v) - 1
        print(f'{a}: {syms[i][1]}+{v - syms[i][0]:#x}' if i >= 0 else f'{a}: ?')
