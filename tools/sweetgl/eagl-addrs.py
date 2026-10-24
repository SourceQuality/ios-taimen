#!/usr/bin/env python3
"""Check eagl.c's QuartzCore addresses (sgl_caches) against an extracted shared cache.

usage: eagl-addrs.py [--verify] CACHE_DIR [eagl.c]

CACHE_DIR holds the split arm64 cache (dyld_shared_cache_arm64, .01, ...,
.symbols); the files are matched by UUID, so the HFS+ hardlink inodes 7-Zip
extracts from "[HFS+ Private Data]" work as they are. --verify first checks
every page against the cache's code signature.

For a cache in sgl_caches, each install guard of eagl.c is replayed (the
format hook and entry hook prologues, tex_fragment_shaders[0x13], the
uniform cases and the slots the texture function fixes fill), with the
QuartzCore symbol at each address. For an unknown cache whose QuartzCore is
the same build as dsc-8's, the entry is derived from dsc-8's by QuartzCore's
segment deltas, checked the same way, and printed for eagl.c. A different
QuartzCore build has to be mapped by hand: docs/taimen-eagl-addresses.md.
"""
import bisect, hashlib, mmap, os, re, struct, sys

# QuartzCore of iOS 16.7.16 (20H392) in the iPhone 8 cache (dsc-8)
QC_UUID = '9e40b2eb260c3af1ae45a1a551a8c1b9'
QC8_TEXT, QC8_DATA_CONST = 0x188280000, 0x1d5b219f0


class Sub:
    def __init__(self, path):
        f = open(path, 'rb')
        self.path, self.mm = path, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        h = self.mm
        mo, mc = struct.unpack_from('<II', h, 0x10)
        self.cs = struct.unpack_from('<QQ', h, 0x28)
        self.lsym = struct.unpack_from('<Q', h, 0x48)[0]
        self.uuid = h[0x58:0x68].hex()
        self.base = struct.unpack_from('<Q', h, 0xe0)[0]
        wo, wc = struct.unpack_from('<II', h, 0x138)
        self.sub = struct.unpack_from('<II', h, 0x188)
        self.sym_uuid = h[0x190:0x1a0].hex()
        self.images = struct.unpack_from('<II', h, 0x1c0)
        self.maps = [struct.unpack_from('<QQQ', h, mo + 32 * i) for i in range(mc)]
        self.slide = [struct.unpack_from('<QQQQQ', h, wo + 56 * i) for i in range(wc)] if wo < mo + 0x1000 else []

    def verify(self):
        """Pages that do not match the code directory's hashes."""
        sb = self.mm[self.cs[0]:self.cs[0] + self.cs[1]]
        for i in range(struct.unpack_from('>I', sb, 8)[0]):
            off = struct.unpack_from('>I', sb, 16 + 8 * i)[0]
            if struct.unpack_from('>I', sb, off)[0] != 0xfade0c02:
                continue
            ver, hoff, ncode, limit = struct.unpack_from('>I4xI8xII', sb, off + 8)
            hsize, htype, _, plog = struct.unpack_from('>BBBB', sb, off + 36)
            if ver >= 0x20300 and struct.unpack_from('>Q', sb, off + 0x38)[0]:
                limit = struct.unpack_from('>Q', sb, off + 0x38)[0]
            algo = {1: 'sha1', 2: 'sha256', 3: 'sha256', 4: 'sha384'}[htype]
            ps = 1 << plog
            return [p for p in range(ncode) if hashlib.new(algo, self.mm[p * ps:min((p + 1) * ps, limit)]).digest()[
                :hsize] != sb[off + hoff + p * hsize:off + hoff + (p + 1) * hsize]]
        return []


class Cache:
    def __init__(self, d):
        subs = []
        for n in sorted(os.listdir(d)):
            p = os.path.join(d, n)
            if os.path.isfile(p) and os.path.getsize(p) >= 0x1000 and open(p, 'rb').read(7) == b'dyld_v1':
                subs.append(Sub(p))
        self.main = next(s for s in subs if s.sub[1] and s.images[1])
        m = self.main
        want = {m.uuid, m.sym_uuid} | {m.mm[m.sub[0] + 56 * i:m.sub[0] + 56 * i + 16].hex() for i in range(m.sub[1])}
        self.subs = [s for s in subs if s.uuid in want]  # not the DriverKit cache next to it
        self.missing = len(want) - len(self.subs)
        self.base = m.base

    def find(self, a):
        for s in self.subs:
            for va, size, fo in s.maps:
                if va <= a < va + size:
                    return s, fo + a - va
        raise KeyError(hex(a))

    def read(self, a, n):
        s, o = self.find(a)
        return bytes(s.mm[o:o + n])

    def ptr(self, a):
        """Unslid target of a rebased pointer (slide info v2, arm64)."""
        raw = struct.unpack('<Q', self.read(a, 8))[0]
        s, _ = self.find(a)
        for va, size, fo, sio, sis in s.slide:
            if va <= a < va + size and sis:
                ver, = struct.unpack_from('<I', s.mm, sio)
                assert ver == 2, 'slide info v%d' % ver
                mask, add = struct.unpack_from('<QQ', s.mm, sio + 24)
                v = raw & ~mask & (1 << 64) - 1
                return v + add if v else 0
        return raw

    def cstr(self, a):
        return self.read(a, 256).split(b'\0', 1)[0].decode('latin-1')

    def image(self, name):
        m = self.main
        for i in range(m.images[1]):
            addr, _, _, po = struct.unpack_from('<QQQI', m.mm, m.images[0] + 32 * i)
            if m.mm[po:m.mm.find(b'\0', po)].decode().endswith('/' + name):
                return addr
        raise KeyError(name)

    def macho(self, mh):
        ncmds, size = struct.unpack('<II', self.read(mh + 16, 8))
        cmds = self.read(mh + 32, size)
        segs, uuid, o = {}, None, 0
        for _ in range(ncmds):
            cmd, n = struct.unpack_from('<II', cmds, o)
            if cmd == 0x19:
                segs[cmds[o + 8:o + 24].rstrip(b'\0').decode()] = struct.unpack_from('<Q', cmds, o + 24)[0]
            elif cmd == 0x1b:
                uuid = cmds[o + 8:o + 24].hex()
            o += n
        return segs, uuid

    def local_symbols(self, mh):
        s = next((s for s in self.subs if s.uuid == self.main.sym_uuid), None)
        if s is None:
            return []
        o = s.lsym
        nlo, _, so, _, eo, ec = struct.unpack_from('<6I', s.mm, o)
        out = []
        for i in range(ec):
            dyoff, start, cnt = struct.unpack_from('<QII', s.mm, o + eo + 16 * i)
            if dyoff + self.base == mh:
                for k in range(start, start + cnt):
                    strx, _, _, _, val = struct.unpack_from('<IBBHQ', s.mm, o + nlo + 16 * k)
                    a = o + so + strx
                    out.append((val, s.mm[a:s.mm.find(b'\0', a)].decode('latin-1')))
        return sorted(out)


def parse_eagl(path):
    src = open(path).read()
    num = lambda s: [int(x, 0) for x in re.findall(r'0x[0-9a-f]+', s)]
    body = re.search(r'\} sgl_caches\[\] = \{(.*?)\n\};', src, re.S).group(1)
    caches = []
    for name, rest in re.findall(r'\{"([^"]+)",(.*?)\}\},', body, re.S):
        v = num(rest)
        caches.append({'name': name, 'uuid': bytes(v[:16]).hex(), 'fmt': v[16], 'tex': v[17], 'cases': v[18],
                       'hook': v[19:]})
    defs = re.search(r'sgl_hook_def\[SGL_HOOKS\] = \{(.*?)\n\};', src, re.S).group(1)
    hooks = [(num(c), n) for c, n in re.findall(r'\{\{([^}]*)\}, "([^"]+)"\}', defs)]
    fmt = num(re.search(r'static const uint32_t want\[4\] = \{([^}]*)\}', src).group(1))
    fixes = [int(x, 0) for x in re.findall(r'\{(0x[0-9a-f]+), "sgl_', src)]
    return caches, hooks, fmt, fixes


def check(c, e, hooks, fmt, fixes, syms):
    addrs = [a for a, _ in syms]

    def name(a):
        i = bisect.bisect_right(addrs, a) - 1
        return '' if i < 0 else syms[i][1] + ('+%#x' % (a - syms[i][0]) if a != syms[i][0] else '')

    bad = 0
    for a, want, what in [(e['fmt'], fmt, 'format hook')] + [(a, w, n) for a, (w, n) in zip(e['hook'], hooks)]:
        ok = list(struct.unpack('<4I', c.read(a, 16))) == want
        bad += not ok
        print('  %-28s %#x %-4s %s' % (what, a, 'ok' if ok else 'FAIL', name(a)))
    t13 = c.ptr(e['tex'] + 8 * 0x13)
    cases = c.read(e['cases'] - 0x11, 0x5a)
    ok = t13 != 0 and c.cstr(t13) == 'colorMatrix4x4Bias(s0)' and cases[0x13] == 0
    bad += not ok
    print('  %-28s %#x %-4s %s' % ('tex_fragment_shaders', e['tex'], 'ok' if ok else 'FAIL', name(e['tex'])))
    print('  %-28s %#x' % ('uniform_cases', e['cases']))
    for fn in fixes:
        ok = c.ptr(e['tex'] + 8 * fn) == 0 and cases[fn] == 0xde
        bad += not ok
        print('    texture function %#x free: %s' % (fn, 'ok' if ok else 'FAIL'))
    return bad


def main():
    args = sys.argv[1:]
    verify = '--verify' in args
    args = [a for a in args if a != '--verify']
    c = Cache(args[0])
    eagl = args[1] if len(args) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'eagl.c')
    caches, hooks, fmt, fixes = parse_eagl(eagl)
    if verify:
        for s in c.subs:
            bad = s.verify()
            if bad:
                print('%s: %d pages do not match the code signature' % (s.path, len(bad)))
                return 1
        print('all pages of %d cache files match their code signatures' % len(c.subs))
    if c.missing:
        print('warning: %d sub-caches of this cache are not in %s' % (c.missing, args[0]))
    qc = c.image('QuartzCore')
    segs, quuid = c.macho(qc)
    print('cache %s, QuartzCore at %#x (LC_UUID %s)' % (c.main.uuid, qc, quuid))
    syms = c.local_symbols(qc)
    e = next((e for e in caches if e['uuid'] == c.main.uuid), None)
    if e is not None:
        print('in sgl_caches as "%s"' % e['name'])
        return 1 if check(c, e, hooks, fmt, fixes, syms) else 0
    if quuid != QC_UUID:
        print('not in sgl_caches, and QuartzCore is another build: map it by hand (docs/taimen-eagl-addresses.md)')
        return 1
    d8 = next(e for e in caches if e['fmt'] == 0x18840433c)
    dt, dd = segs['__TEXT'] - QC8_TEXT, segs['__DATA_CONST'] - QC8_DATA_CONST
    e = {'name': '?', 'uuid': c.main.uuid, 'fmt': d8['fmt'] + dt, 'tex': d8['tex'] + dd, 'cases': d8['cases'] + dt,
         'hook': [a + dt for a in d8['hook']]}
    print('not in sgl_caches; same QuartzCore as dsc-8 at __TEXT %+#x, __DATA_CONST %+#x' % (dt, dd))
    if check(c, e, hooks, fmt, fixes, syms):
        return 1
    u = bytes.fromhex(c.main.uuid)
    print('\nentry for sgl_caches:\n\t{"NAME",\n\t    {%s},\n\t    %#x, %#x, %#x,\n\t    {%s,\n\t    %s}},' % (
        ', '.join('%#04x' % b for b in u), e['fmt'], e['tex'], e['cases'],
        ', '.join('%#x' % a for a in e['hook'][:8]), ', '.join('%#x' % a for a in e['hook'][8:])))
    return 0


if __name__ == '__main__':
    sys.exit(main())
