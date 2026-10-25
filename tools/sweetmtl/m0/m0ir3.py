#!/usr/bin/env python3
# M0 step 2: compile every SPIR-V stage m0drive.py wrote with spv2ir3 for gpu 540; write
# out/ir3/<pipeline>.<stage>.txt (disassembly) and out/m0-ir3.json, print a summary.
#   tools/sweetmtl/m0/m0ir3.py [-j N] [--gpu 540]
import argparse, collections, concurrent.futures as cf, glob, json, os, re, subprocess

H = os.path.dirname(os.path.abspath(__file__))
M0 = os.environ.get('M0') or os.path.normpath(H + '/../../../../research/m0')
TOOL = M0 + '/mesa-host/src/freedreno/ir3/spv2ir3'


def stage_of(spv):
    b = os.path.basename(spv)
    return 'vert' if '.vert.' in b else 'frag' if '.frag.' in b else 'comp'


def one(spv, gpu, out):
    st = stage_of(spv)
    name = os.path.basename(os.path.dirname(spv)) + '.' + os.path.basename(spv)[:-4]
    try:
        p = subprocess.run([TOOL, '--gpu', str(gpu), st, spv], capture_output=True, text=True,
                           timeout=120)
        txt, rc = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired:
        txt, rc = 'TIMEOUT', -1
    open(out + '/' + name + '.txt', 'w').write(txt)
    m = re.search(r'^OK .*$', txt, re.M)
    r = {'name': name, 'stage': st, 'rc': rc, 'ok': bool(m) and rc == 0,
         'summary': m.group(0) if m else None,
         'raw': len(re.findall(r'^\s*raw 0x', txt, re.M))}
    if not r['ok']:
        lines = [l for l in txt.splitlines() if l.strip()]
        r['error'] = ' | '.join(lines[-4:])[:600] if lines else 'rc %d, no output' % rc
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-j', type=int, default=os.cpu_count())
    ap.add_argument('--gpu', type=int, default=540)
    a = ap.parse_args()
    out = M0 + '/out/ir3-%d' % a.gpu
    os.makedirs(out, exist_ok=True)
    spvs = sorted(glob.glob(M0 + '/out/pipelines/*/*.spv'))
    with cf.ThreadPoolExecutor(a.j) as ex:
        res = list(ex.map(lambda s: one(s, a.gpu, out), spvs))
    json.dump(res, open(M0 + '/out/m0-ir3-%d.json' % a.gpu, 'w'), indent=1)
    ok = [r for r in res if r['ok']]
    print('%d/%d stages compile for gpu %d; %d with raw (undecoded) words' %
          (len(ok), len(res), a.gpu, sum(1 for r in ok if r['raw'])))
    errs = collections.Counter()
    for r in res:
        if not r['ok']:
            e = re.sub(r'0x[0-9a-f]+|\d+', 'N', r['error'])
            errs[e[:200]] += 1
    for e, n in errs.most_common(30):
        print('%4d  %s' % (n, e))
    if ok:
        ins = sorted(int(re.search(r'instrs=(\d+)', r['summary']).group(1)) for r in ok)
        print('instrs: median %d, max %d' % (ins[len(ins) // 2], ins[-1]))


if __name__ == '__main__':
    main()
