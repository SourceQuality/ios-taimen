#!/usr/bin/env python3
"""Sum the PROF samples of a sweet log (bsd/dev/sweet_sample.c) per function.

  prof-sym.py LOG NAME [--cache DIR --slide HEX] [--dylib PATH --base HEX] [--kernel PATH]
              [--from S] [--to S] [--top N]

NAME is the profiled process (PROF NAME[pid] round ... headers). Untagged pcs are
where the samples landed (self); tagged ones (low 2 bits: 1 for user frames, the depth
1-3 for kernel frames) are callers found by the frame-pointer walk (inclusive). Kernel pcs are unslid: named from the kernel
Mach-O's symbols. Shared-cache pcs need the cache (tools/sweetgl/dsc-sym.py) and the
process's slide; the dylib's need its load base (sweetgl logs "loaded at"). --from/--to
keep the rounds whose header time ([t] prefix, or the n-th round x 10 s) is in range.
"""
import argparse, bisect, collections, importlib.util, os, re, struct


def macho_syms(path):
    data = open(path, 'rb').read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, 0)
    assert magic == 0xfeedfacf, hex(magic)
    off, text, out = 32, None, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', data, off)
        if cmd == 0x19 and data[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            text = struct.unpack_from('<Q', data, off + 24)[0]
        if cmd == 0x2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, off + 8)
            for i in range(nsyms):
                strx, typ, sect, _, val = struct.unpack_from('<IBBHQ', data, symoff + 16 * i)
                if typ & 0xe0 or (typ & 0x0e) != 0x0e or val == 0:
                    continue
                e = data.index(b'\0', stroff + strx)
                out.append((val, data[stroff + strx:e].decode('latin-1')))
        off += size
    out.sort()
    return text, out, [a for a, _ in out]


def lookup(table, a, bias=0):
    _, syms, addrs = table
    i = bisect.bisect_right(addrs, a - bias) - 1
    return syms[i][1] if i >= 0 else '?'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('name')
    ap.add_argument('--cache')
    ap.add_argument('--slide', type=lambda s: int(s, 16), default=0)
    ap.add_argument('--dylib')
    ap.add_argument('--base', type=lambda s: int(s, 16))
    ap.add_argument('--kernel')
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()

    self_n, caller_n = collections.Counter(), collections.Counter()
    rounds = samples = kernel = 0
    on = False
    for line in open(a.log, errors='replace'):
        m = re.search(r'PROF (\S+)\[\d+\] round \d+ samples (\d+) kernel (\d+) of all (\d+)', line)
        if m:
            on = m.group(1) == a.name
            if on:
                rounds += 1
                samples += int(m.group(2))
                kernel += int(m.group(3))
            continue
        if on and line.startswith('PROF '):
            for pc, n in re.findall(r'([0-9a-f]{4,}):(\d+)', line):
                pc = int(pc, 16)
                (caller_n if pc & 3 else self_n)[pc & ~3] += int(n)
    print('%s: %d rounds, %d samples, %d in the kernel' % (a.name, rounds, samples, kernel))

    kern = macho_syms(a.kernel) if a.kernel else None
    dy = macho_syms(a.dylib) if a.dylib else None
    dsc = None
    if a.cache:
        spec = importlib.util.spec_from_file_location(
            'dsc_sym', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dsc-sym.py'))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        dsc = mod.Images(mod.ea.Cache(a.cache))

    def name(pc):
        if pc >= 0xfffffff000000000:
            return 'kernel`' + lookup(kern, pc) if kern else 'kernel'
        if dy and a.base is not None and a.base <= pc < a.base + 0x4000000:
            return 'sweetgl`' + lookup(dy, pc, a.base - dy[0])
        if 0x180000000 <= pc - a.slide < 0x300000000:
            n = dsc.name(pc - a.slide) if dsc else None
            return (n or 'cache?').split('+0x')[0]
        return '?%#x' % pc

    for title, cnt, total in (('self', self_n, samples), ('callers (inclusive)', caller_n, samples)):
        by = collections.Counter()
        for pc, n in cnt.items():
            by[name(pc)] += n
        print('\n%s:' % title)
        for fn, n in by.most_common(a.top):
            print('%6d %5.1f%%  %s' % (n, 100.0 * n / max(total, 1), fn))


if __name__ == '__main__':
    main()
