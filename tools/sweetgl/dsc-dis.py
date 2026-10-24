#!/usr/bin/env python3
"""Disassemble one function of an extracted shared cache, branch targets named.

  dsc-dis.py CACHE_DIR IMAGE SYMBOL_SUBSTRING [MAX_BYTES]

Unslid addresses. Needs aarch64-linux-gnu-objdump (binutils-aarch64-linux-gnu).
Calls (bl/b) and adrp+add/ldr pairs are annotated with the symbol or C string
they reach.
"""
import bisect, importlib.util, os, re, subprocess, sys, tempfile

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('dsc_sym', os.path.join(here, 'dsc-sym.py'))
ds = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ds)


def main():
    c = ds.ea.Cache(sys.argv[1])
    im = ds.Images(c)
    image, want = sys.argv[2], sys.argv[3]
    limit = int(sys.argv[4], 0) if len(sys.argv) > 4 else 0x2000
    mh = next(t[2] for t in im.text if t[3] == image)
    syms, addrs = im.symbols(mh)
    hits = [(a, n) for a, n in syms if want in n]
    if not hits:
        sys.exit('no symbol with %r in %s' % (want, image))
    for a, n in hits[:8]:
        print('# %#x %s' % (a, n))
    start, name = hits[0]
    i = bisect.bisect_right(addrs, start)
    end = addrs[i] if i < len(addrs) else start + limit
    end = min(end, start + limit)
    code = c.read(start, end - start)
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        f.write(code)
    out = subprocess.run(['aarch64-linux-gnu-objdump', '-D', '-b', 'binary', '-m', 'aarch64',
                          '--adjust-vma=%#x' % start, f.name], capture_output=True, text=True).stdout
    os.unlink(f.name)
    print('# %s: %#x-%#x' % (name, start, end))
    pages = {}
    for line in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s+[0-9a-f]{8}\s+(.*)', line)
        if not m:
            continue
        pc, ins = int(m.group(1), 16), m.group(2)
        note = ''
        b = re.match(r'(bl|b|b\.\w+|cbz|cbnz|tbz|tbnz)\s.*?0x([0-9a-f]+)', ins)
        if b and b.group(1) in ('bl', 'b'):
            t = int(b.group(2), 16)
            if not start <= t < end:
                note = im.name(t) or ''
        a = re.match(r'adrp\s+(x\d+),\s*0x([0-9a-f]+)', ins)
        if a:
            pages[a.group(1)] = int(a.group(2), 16)
        a = re.match(r'(add|ldr|ldrsw)\s+(x\d+|w\d+),\s*\[?(x\d+)(?:,\s*#(0x[0-9a-f]+|\d+))?', ins)
        if a and a.group(3) in pages:
            off = int(a.group(4), 0) if a.group(4) else 0
            t = pages[a.group(3)] + off
            if a.group(1) == 'add':
                try:
                    s = c.cstr(t)
                    note = repr(s[:60]) if s and all(32 <= ord(ch) < 127 for ch in s[:20]) else (im.name(t) or '')
                except KeyError:
                    note = ''
            else:
                note = '[%s]' % (im.name(t) or hex(t))
        print('%x: %-48s %s' % (pc, ins, note))


if __name__ == '__main__':
    main()
