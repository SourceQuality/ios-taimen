#!/usr/bin/env python3
"""Symbolicate slid shared-cache addresses with an extracted (split) cache.

  dsc-sym.py CACHE_DIR SLIDE [ADDR...]       (addresses on stdin if none given)

CACHE_DIR as for tools/sweetgl/eagl-addrs.py (files matched by UUID). SLIDE is
the shared region slide of the process (SMP/UBT/CRASH header lines). Prints
"ADDR image`symbol+off" per address; addresses outside the cache as they are.
"""
import bisect, importlib.util, os, struct, sys

spec = importlib.util.spec_from_file_location(
    'eagl_addrs', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'eagl-addrs.py'))
ea = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ea)


class Images:
    def __init__(self, c):
        self.c, m = c, c.main
        self.text = []          # (start, end, mh, name)
        for i in range(m.images[1]):
            mh, _, _, po = struct.unpack_from('<QQQI', m.mm, m.images[0] + 32 * i)
            name = m.mm[po:m.mm.find(b'\0', po)].decode().rsplit('/', 1)[-1]
            try:
                segs = self.segs(mh)
            except KeyError:
                continue
            if '__TEXT' in segs:
                va, size, _ = segs['__TEXT']
                self.text.append((va, va + size, mh, name))
        self.text.sort()
        self.starts = [t[0] for t in self.text]
        self.syms = {}

    def segs(self, mh):
        ncmds, size = struct.unpack('<II', self.c.read(mh + 16, 8))
        cmds = self.c.read(mh + 32, size)
        out, o = {}, 0
        self.symtab = None
        for _ in range(ncmds):
            cmd, n = struct.unpack_from('<II', cmds, o)
            if cmd == 0x19:
                name = cmds[o + 8:o + 24].rstrip(b'\0').decode()
                va, vs, fo = struct.unpack_from('<QQQ', cmds, o + 24)
                out[name] = (va, vs, fo)
            elif cmd == 0x2:
                self.symtab = struct.unpack_from('<IIII', cmds, o + 8)
            o += n
        return out

    def symbols(self, mh):
        if mh in self.syms:
            return self.syms[mh]
        out = list(self.c.local_symbols(mh))
        try:
            segs = self.segs(mh)
            if self.symtab and '__LINKEDIT' in segs:
                lva, _, lfo = segs['__LINKEDIT']
                symoff, nsyms, stroff, strsize = self.symtab
                tab = self.c.read(lva + symoff - lfo, 16 * nsyms)
                strs = self.c.read(lva + stroff - lfo, strsize)
                for k in range(nsyms):
                    strx, typ, sect, _, val = struct.unpack_from('<IBBHQ', tab, 16 * k)
                    if sect and val and not typ & 0xe0:
                        out.append((val, strs[strx:strs.find(b'\0', strx)].decode('latin-1')))
        except (KeyError, struct.error):
            pass
        out.sort()
        self.syms[mh] = (out, [a for a, _ in out])
        return self.syms[mh]

    def name(self, a):
        i = bisect.bisect_right(self.starts, a) - 1
        if i < 0 or a >= self.text[i][1]:
            return None
        _, _, mh, img = self.text[i]
        syms, addrs = self.symbols(mh)
        j = bisect.bisect_right(addrs, a) - 1
        if j < 0:
            return '%s+%#x' % (img, a - mh)
        return '%s`%s+%#x' % (img, syms[j][1], a - syms[j][0])


def main():
    c = ea.Cache(sys.argv[1])
    slide = int(sys.argv[2], 16)
    addrs = sys.argv[3:] or sys.stdin.read().split()
    im = Images(c)
    for s in addrs:
        a = int(s.split(':')[0], 16)
        n = im.name((a & ~3) - slide)
        print('%s %s' % (s, n or ''))


if __name__ == '__main__':
    main()
