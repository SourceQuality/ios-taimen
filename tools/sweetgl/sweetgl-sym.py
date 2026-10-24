#!/usr/bin/env python3
"""Name sweetgl.dylib addresses from an SMP/UBT backtrace (bsd/dev/sweet_sample.c lines).

  sweetgl-sym.py sweetgl.dylib THREAD_START_RA [ADDR ...]

Reads the Mach-O LC_SYMTAB itself (WSL binutils do not read Mach-O). The dylib load base is
found from the first address, the return address in impl_thrd_routine at the bottom of a Mesa
worker thread (16 KB aligned); the rest are then named function+offset (boot 61).
"""
import struct, sys, bisect
data = open(sys.argv[1], 'rb').read()
magic, cpu, sub, ftype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<8I', data, 0)
assert magic == 0xfeedfacf, hex(magic)
off = 32
syms = []
text_vm = None
for _ in range(ncmds):
    cmd, size = struct.unpack_from('<2I', data, off)
    if cmd == 0x19:  # LC_SEGMENT_64
        name = data[off + 8:off + 24].rstrip(b'\0').decode()
        vmaddr, vmsize = struct.unpack_from('<2Q', data, off + 24)
        if name == '__TEXT':
            text_vm = vmaddr
    if cmd == 0x2:   # LC_SYMTAB
        symoff, nsyms, stroff, strsize = struct.unpack_from('<4I', data, off + 8)
        for i in range(nsyms):
            strx, ntype, nsect, ndesc, nvalue = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
            if ntype & 0xe0:      # stab
                continue
            if (ntype & 0x0e) != 0x0e or nvalue == 0:   # N_SECT only
                continue
            end = data.index(b'\0', stroff + strx)
            syms.append((nvalue, data[stroff + strx:end].decode(errors='replace')))
    off += size
syms.sort()
addrs = [s[0] for s in syms]
print('text vmaddr', hex(text_vm), 'symbols', len(syms), file=sys.stderr)
def name(a):
    i = bisect.bisect_right(addrs, a) - 1
    return '%s+%#x' % (syms[i][1], a - syms[i][0]) if i >= 0 else '?'
args = [int(x, 16) for x in sys.argv[2:]]
if args:
    # the load base: 16 KB aligned, such that the thread start (first arg) lands in impl_thrd_routine
    starts = [s for s in syms if 'impl_thrd_routine' in s[1]]
    print('thread start candidates', [(hex(a), n) for a, n in starts], file=sys.stderr)
    base = None
    for a, n in starts:
        cand = (args[0] - a) & ~0x3fff
        if 0 <= args[0] - cand - a < 0x400:
            base = cand
            break
    if base is None:
        print('no base found'); sys.exit(1)
    print('load base', hex(base))
    for x in args:
        print(hex(x), name(x - base))
