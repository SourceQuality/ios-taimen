#!/usr/bin/env python3
# M0 driver: translate every CoreAnimation pipeline of default.metallib with metal2vulkan
# against the a540 profile. Vertex descriptors come from m0dump's reflection (one interleaved
# buffer at index 30); each fragment is paired with vertex functions by name first, then any
# vertex function of the same precision, until one links.
#   tools/sweetmtl/m0/m0drive.py [-j N] [--only REGEX]
# Reads $M0/default.metallib (QuartzCore's, from the iOS root; never in git); writes
# $M0/out/pipelines/<pipeline>/ and $M0/out/m0-report.json. M0 defaults to ../research/m0.
import argparse, concurrent.futures as cf, json, os, re, subprocess, sys

H = os.path.dirname(os.path.abspath(__file__))
M0 = os.environ.get('M0') or os.path.normpath(H + '/../../../../research/m0')
M2V = M0 + '/metal2vulkan/target/release/metal2vulkan'
M0DUMP = M0 + '/m0dump-target/release/m0dump'
LIB = M0 + '/default.metallib'
PROFILE = H + '/a540-gallium.json'
OUT = M0 + '/out/pipelines'

FMT = {  # MTLVertexFormat for an attribute type, and its size
    'Float': (28, 4), 'Float2': (29, 8), 'Float3': (30, 12), 'Float4': (31, 16),
    'Half': (53, 2), 'Half2': (25, 4), 'Half3': (26, 6), 'Half4': (27, 8),
    'Int': (32, 4), 'Int2': (33, 8), 'Int3': (34, 12), 'Int4': (35, 16),
    'UInt': (36, 4), 'UInt2': (37, 8), 'UInt3': (38, 12), 'UInt4': (39, 16),
    'Short': (50, 2), 'Short2': (16, 4), 'Short3': (17, 6), 'Short4': (18, 8),
    'UShort': (49, 2), 'UShort2': (13, 4), 'UShort3': (14, 6), 'UShort4': (15, 8),
    'Char': (46, 1), 'Char2': (4, 2), 'Char3': (5, 3), 'Char4': (6, 4),
    'UChar': (45, 1), 'UChar2': (1, 2), 'UChar3': (2, 3), 'UChar4': (3, 4),
}
VBUF = 30


def load():
    if not os.path.exists(M0 + '/functions.jsonl'):
        with open(M0 + '/functions.jsonl', 'w') as f:
            subprocess.run([M0DUMP, LIB], stdout=f, check=True)
    fns = {}
    for line in open(M0 + '/functions.jsonl'):
        j = json.loads(line)
        name = re.search(r'name: "([^"]+)"', j['info']).group(1)
        stage = re.search(r'stage: (\w+)', j['info']).group(1)
        fns[name] = dict(stage=stage, props=j['properties'], consts=j['constants'])
    return fns


def vdesc(fn, path):
    attrs = fn['props'].get('vertex_attributes') or []
    off, a = 0, {}
    for at in attrs:
        fmt, size = FMT[at['attribute_type']]
        a[str(at['attribute_index'])] = {'format': at['attribute_type'], 'offset': off, 'buffer_index': VBUF}
        off += (size + 3) & ~3
    d = {'layouts': {str(VBUF): {'stride': max(off, 4), 'step_function': 'PerVertex', 'step_rate': 1}} if a else {},
         'attributes': a}
    json.dump(d, open(path, 'w'))
    return path


def suffix(name):
    m = re.search(r'_(lp[fh])(_\w+)?$', name)
    return m.group(1) if m else None


def candidates(frag, verts):
    s = suffix(frag)
    same = [v for v in verts if suffix(v) == s] or list(verts)
    base = re.sub(r'_frag.*|_lp[fh].*', '', frag)
    named = []
    for v in same:
        vb = re.sub(r'_vert.*', '', v)
        if vb and (base.startswith(vb) or vb.startswith(base)):
            named.append(v)
    named.sort(key=lambda v: -len(re.sub(r'_vert.*', '', v)))
    prefer = [v for v in same if v.startswith(('std_vert1', 'std_vert0', 'fixed_vert'))]
    seen, out = set(), []
    for v in named + prefer + same:
        if v not in seen:
            seen.add(v); out.append(v)
    return out


def run(args, timeout=120):
    try:
        p = subprocess.run(args, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr).strip()
    except subprocess.TimeoutExpired:
        return -1, 'TIMEOUT'


def classify(msg):
    for k in ('FALLBACK', 'unsupported', 'Unsupported', 'refused', 'error', 'TIMEOUT'):
        if k in msg:
            return msg.splitlines()[-1][:300] if msg else k
    return msg[:300]


CTYPE = {'Bool': 'bool', 'UChar': 'uchar', 'Char': 'char', 'UShort': 'ushort', 'Short': 'short',
         'UInt': 'uint', 'Int': 'int', 'Float': 'float', 'Half': 'half'}


def constants(names, fns):
    # A value for every required function constant: fc_generic by the vertex function's
    # name (_gen), other bools true, integers 1.
    # Others the compile asks for (index in `extra`): bools false, integers 0.
    return const_args(names, fns, set())


def const_value(cn, t, names, required):
    if t == 'Bool':
        if cn == 'fc_generic':
            return 'true' if any(x.endswith('_gen') for x in names) else 'false'
        return 'true' if required else 'false'
    return '1' if required else '0'


def const_args(names, fns, extra):
    out, seen = [], set()
    for n in names:
        for cn, c in (fns[n]['consts'] or {}).items():
            if cn in seen or not (c.get('required') or c['index'] in extra):
                continue
            seen.add(cn)
            t = c['data_type']
            out += ['--constant', '%s:%s:%s' % (cn, CTYPE.get(t, t.lower()),
                                                const_value(cn, t, names, c.get('required')))]
    return out


COLOR_FORMATS = ['BGRA8Unorm', 'BGRA8Unorm', 'R32Uint', 'R16Float', 'R32Float', 'RGBA16Float',
                 'R32Sint', 'R16Uint', 'R8Uint', 'RG16Float', 'RGBA32Float', 'R8Unorm']


def compile_render(vert, frag, fns, tag):
    d = OUT + '/' + tag
    os.makedirs(d, exist_ok=True)
    vd = vdesc(fns[vert], d + '/vdesc.json')
    names = [vert] + ([frag] if frag else [])
    extra, fmts = set(), ['BGRA8Unorm']
    tried = {}
    for _ in range(60):
        args = [M2V, LIB, d + '/p.spv', '--profile', PROFILE, '--function', vert,
                '--vertex-descriptor', vd, '--emit-meta', d + '/meta.json', '--no-spirv-val']
        if frag:
            args += ['--fragment', frag]
            for i, f in enumerate(fmts):
                args += ['--color-format', '%d:%s' % (i, f)]
        args += const_args(names, fns, extra)
        rc, msg = run(args)
        ok = rc == 0 and 'FALLBACK' not in msg
        m = re.search(r'needs pipeline input: FunctionConstant \{ index: (\d+) \}', msg)
        if not ok and m and int(m.group(1)) not in extra:
            extra.add(int(m.group(1)))
            continue
        m = re.search(r'invalid descriptor: ColorAttachment \{ index: (\d+) \}', msg)
        if not ok and frag and m:
            i = int(m.group(1))
            while len(fmts) <= i:
                fmts.append('BGRA8Unorm')
            n = tried.get(i, 0) + 1
            tried[i] = n
            if n < len(COLOR_FORMATS):
                fmts[i] = COLOR_FORMATS[n]
                continue
        break
    open(d + '/args.txt', 'w').write(' '.join(args[1:]) + '\n')
    return ok, msg


def do_frag(frag, verts, fns):
    tries = []
    for v in candidates(frag, verts)[:8]:
        ok, msg = compile_render(v, frag, fns, frag + '+' + v)
        tries.append((v, ok, msg))
        if ok:
            break
        if 'interface' not in msg.lower() and 'link' not in msg.lower() and 'varying' not in msg.lower() \
                and 'input' not in msg.lower():
            break  # the fragment itself fails, not the pairing
    return frag, tries


def do_vert(vert, fns):
    ok, msg = compile_render(vert, None, fns, vert)
    return vert, [(vert, ok, msg)]


def do_kernel(k, fns):
    d = OUT + '/' + k
    os.makedirs(d, exist_ok=True)
    rc, msg = run([M2V, LIB, d + '/p.spv', '--profile', PROFILE, '--function', k,
                   '--emit-meta', d + '/meta.json', '--no-spirv-val'])
    ok = rc == 0 and 'FALLBACK' not in msg
    tries = [(k, ok, msg)]
    if not ok:
        rc, msg2 = run([M2V, LIB, d + '/tile.spv', '--profile', PROFILE, '--function', k,
                        '--color-format', '0:BGRA8Unorm', '--no-spirv-val'])
        tries.append((k + '(tile)', rc == 0 and 'FALLBACK' not in msg2, msg2))
    return k, tries


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=os.cpu_count())
    ap.add_argument('--only')
    a = ap.parse_args()
    fns = load()
    verts = [n for n, f in fns.items() if f['stage'] == 'Vertex']
    frags = [n for n, f in fns.items() if f['stage'] == 'Fragment']
    kerns = [n for n, f in fns.items() if f['stage'] not in ('Vertex', 'Fragment')]
    if a.only:
        r = re.compile(a.only)
        verts2 = [v for v in verts if r.search(v)]; frags = [f for f in frags if r.search(f)]
        kerns = [k for k in kerns if r.search(k)]
    else:
        verts2 = verts
    jobs = []
    with cf.ThreadPoolExecutor(a.j) as ex:
        jobs += [ex.submit(do_vert, v, fns) for v in verts2]
        jobs += [ex.submit(do_frag, f, verts, fns) for f in frags]
        jobs += [ex.submit(do_kernel, k, fns) for k in kerns]
        res = [j.result() for j in jobs]
    report = {}
    good = 0
    for name, tries in res:
        ok = any(t[1] for t in tries)
        good += ok
        report[name] = {'ok': ok, 'stage': fns[name]['stage'],
                        'tries': [{'with': v, 'ok': o, 'msg': m[-2000:]} for v, o, m in tries]}
        last = tries[-1]
        print(('OK  ' if ok else 'FAIL') + ' %-45s %s' % (name, '' if ok else classify(last[2])))
    json.dump(report, open(M0 + '/out/m0-report.json', 'w'), indent=1)
    print('%d/%d ok' % (good, len(res)))


if __name__ == '__main__':
    main()
